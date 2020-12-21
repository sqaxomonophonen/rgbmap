// boilerplate
#define _XOPEN_SOURCE 700
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ftw.h>
#include <math.h>
#include "stb_image.h"
#include "stb_image_write.h"

// configuration
#define PALETTE_DEPTH_LOG2 (5)
#define PALETTE_COLS       (8)
#define PALETTE_ROWS       (4)
static char* jpeg_extensions[] = {"jpg", "jpeg", 0};
static char* png_extensions[] = {"png", 0};

#define DEBUG

// globals
char* prg;
uint8_t* lut;
char* src_dir;
char* dst_dir;

// derived config
#define PALETTE_DEPTH      (1 << PALETTE_DEPTH_LOG2)
#define PALETTE_MASK       (PALETTE_DEPTH - 1)
#define PALETTE_WIDTH      (PALETTE_DEPTH * PALETTE_COLS)
#define PALETTE_HEIGHT     (PALETTE_DEPTH * PALETTE_ROWS)
#define LUT_N              (N_CHANNELS << (PALETTE_DEPTH_LOG2*3))

#define N_CHANNELS (3) // we only support RGB images, so things will probably break if you change this

#ifdef DEBUG
#define DEBUG_ASSERT(x) assert(x)
#else
#define DEBUG_ASSERT(x)
#endif

static void usage_sf(int status, FILE* f)
{
	fprintf(stderr, "Usage: %s <cmd> [options...]\n", prg);
	fprintf(stderr, "\n");
	//               01234567890123456789012345678901234567890123456789012345678901234567890123456789
	fprintf(stderr, "First prepare a reference image; this will contain a color wheel:\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  $ %s prep <input image> <output png>\n", prg);
	fprintf(stderr, "\n");
	fprintf(stderr, "Then color correct the above output image. After doing that you can use it as\n");
	fprintf(stderr, "<reference image> for the following command, which will then apply the same\n");
	fprintf(stderr, "color correction to all images in <src dir> and write them to <dst dir>:\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  $ %s run <reference image> <src dir> <dst dir>\n", prg);
	fprintf(stderr, "\n");
	exit(status);
}

static void usage()
{
	usage_sf(EXIT_FAILURE, stderr);
}


static inline void palette_xy_to_rgbi(int x, int y, int* ri, int* gi, int* bi)
{
	DEBUG_ASSERT(x >= 0);
	DEBUG_ASSERT(y >= 0);
	DEBUG_ASSERT(x < PALETTE_WIDTH);
	DEBUG_ASSERT(y < PALETTE_HEIGHT);

	int qx = x >> PALETTE_DEPTH_LOG2;
	int qy = y >> PALETTE_DEPTH_LOG2;
	DEBUG_ASSERT(qx >= 0);
	DEBUG_ASSERT(qy >= 0);
	DEBUG_ASSERT(qx < PALETTE_COLS);
	DEBUG_ASSERT(qy < PALETTE_ROWS);

	*ri = x & PALETTE_MASK;
	*gi = y & PALETTE_MASK;
	*bi = qx + qy * PALETTE_COLS;

	DEBUG_ASSERT(*ri >= 0);
	DEBUG_ASSERT(*ri < PALETTE_DEPTH);
	DEBUG_ASSERT(*gi >= 0);
	DEBUG_ASSERT(*gi < PALETTE_DEPTH);
	DEBUG_ASSERT(*bi >= 0);
	DEBUG_ASSERT(*bi < PALETTE_DEPTH);
}

static inline uint8_t int2u8(int i)
{
	if (i < 0) return 0;
	if (i > 255) return 255;
	return i;
}

static inline uint8_t palette_index_to_u8(int index)
{
	return (index * 255) / (PALETTE_DEPTH-1);
}

static inline float palette_u8_to_float_index(uint8_t v)
{
	float f = (float)(v * (PALETTE_DEPTH-1)) / 255.0f;
	DEBUG_ASSERT(f >= 0.0f);
	DEBUG_ASSERT(f <= (float)(PALETTE_DEPTH-1));
	return f;
}

static inline void palette_xy_to_rgb(int x, int y, uint8_t* r, uint8_t* g, uint8_t* b)
{
	int ri, gi, bi;
	palette_xy_to_rgbi(x, y, &ri, &gi, &bi);
	*r = palette_index_to_u8(ri);
	*g = palette_index_to_u8(gi);
	*b = palette_index_to_u8(bi);
}

static void palette_sanity_check()
{
	assert((PALETTE_COLS * PALETTE_ROWS) == PALETTE_DEPTH);
}

static void sanitize_dir(char* path)
{
	size_t path_sz = strlen(path);
	for (int i = path_sz-1; i >= 0; i--) {
		if (path[i] == '/') {
			path[i] = 0;
		} else {
			return;
		}
	}
}

static int has_ext(const char* path, char* ext)
{
	size_t path_len = strlen(path);
	size_t ext_len = strlen(ext);
	if (path_len <= (ext_len+1)) return 0;
	if (path[path_len - ext_len - 1] != '.') return 0;
	for (int i = 0; i < ext_len; i++) {
		char a = path[path_len - ext_len + i];
		char b = ext[i];
		if (a != b && a != (b&~32)) return 0;
	}
	return 1;
}

static int has_any_ext(const char* path, char** exts)
{
	for (char** ext = exts; *ext; ext++) {
		if (has_ext(path, *ext)) return 1;
	}
	return 0;
}

static inline int lut_index(int ri, int gi, int bi)
{
	int index = N_CHANNELS * (ri + (gi << PALETTE_DEPTH_LOG2) + (bi << (PALETTE_DEPTH_LOG2*2)));
	DEBUG_ASSERT(index >= 0);
	DEBUG_ASSERT(index < LUT_N);
	return index;
}

static inline void lut_lookup(int ri, int gi, int bi, int* r, int* g, int* b)
{
	int luti = lut_index(ri, gi, bi);
	uint8_t* lut_pixel = &lut[luti];
	*r = lut_pixel[0];
	*g = lut_pixel[1];
	*b = lut_pixel[2];
}

static inline int palette_xy_to_lut_index(int x, int y)
{
	int ri, gi, bi;
	palette_xy_to_rgbi(x, y, &ri, &gi, &bi);
	return lut_index(ri, gi, bi);
}

// linear interpolation
static inline float lerp1d(float x, float v0, float v1)
{
	return v0*(1.0f-x) + v1*x;
}

// bilinear interpolation
static inline float lerp2d(float x, float y, float v0, float v1, float v2, float v3)
{
	float s = lerp1d(x, v0, v1);
	float t = lerp1d(x, v2, v3);
	return lerp1d(y, s, t);
}

// trilinear interpolation
static inline float lerp3d(float x, float y, float z, float v0, float v1, float v2, float v3, float v4, float v5, float v6, float v7)
{
	float s = lerp2d(x, y, v0, v1, v2, v3);
	float t = lerp2d(x, y, v4, v5, v6, v7);
	return lerp1d(z, s, t);
}

static inline uint8_t f2u8(float f)
{
	int i = f;
	if (i < 0) return 0;
	if (i > 255) return 255;
	return i;
}

static inline void map_pixel(uint8_t* pixel)
{
	float rf = palette_u8_to_float_index(pixel[0]);
	float gf = palette_u8_to_float_index(pixel[1]);
	float bf = palette_u8_to_float_index(pixel[2]);

	float rff = floorf(rf);
	float gff = floorf(gf);
	float bff = floorf(bf);

	int ri = (int)rff;
	int gi = (int)gff;
	int bi = (int)bff;

	float rfr = rf - rff;
	float gfr = bf - bff;
	float bfr = gf - gff;

	if (ri >= (PALETTE_DEPTH-1)) {
		ri--;
		rfr = 1.0f;
	}

	if (gi >= (PALETTE_DEPTH-1)) {
		gi--;
		gfr = 1.0f;
	}

	if (bi >= (PALETTE_DEPTH-1)) {
		bi--;
		bfr = 1.0f;
	}

	int cc[2*2*2*N_CHANNELS]; // color cube
	int* ccp = cc;
	for (int db = 0; db < 2; db++) {
		for (int dg = 0; dg < 2; dg++) {
			for (int dr = 0; dr < 2; dr++) {
				lut_lookup(ri+dr, gi+dg, bi+db, &ccp[0], &ccp[1], &ccp[2]);
				ccp += N_CHANNELS;
			}
		}
	}

	for (int i = 0; i < 3; i++) {
		float v = lerp3d(rfr, gfr, bfr,
			cc[0*N_CHANNELS+i],
			cc[1*N_CHANNELS+i],
			cc[2*N_CHANNELS+i],
			cc[3*N_CHANNELS+i],
			cc[4*N_CHANNELS+i],
			cc[5*N_CHANNELS+i],
			cc[6*N_CHANNELS+i],
			cc[7*N_CHANNELS+i]);
		pixel[i] = f2u8(v);
	}
}

static void makedirs(char* path)
{
	char* p = strdup(path);
	assert(p != NULL);
	char* c = p;
	for (;;) {
		if (*c == 0) break;
		if (*c == '/') {
			*c = 0;
			mkdir(p, 0777); // ignoring errors, woo
			*c = '/';
		}
		c++;
	}
	free(p);
}

static int visit(const char* src_path, const struct stat* st, const int typeflag, struct FTW* pathinfo)
{
	if (typeflag != FTW_F) return 0;

	int is_jpeg = 0;
	int is_png = 0;
	if (has_any_ext(src_path, jpeg_extensions)) {
		is_jpeg = 1;
	} else if (has_any_ext(src_path, png_extensions)) {
		is_png = 1;
	} else {
		return 0;
	}

	DEBUG_ASSERT(is_jpeg || is_png);

	int width, height, n_channels;
	uint8_t* im = stbi_load(src_path, &width, &height, &n_channels, N_CHANNELS);
	if (im == NULL) {
		printf("%s: read failed\n", src_path);
		return 0;
	}
	assert(n_channels == N_CHANNELS);

	uint8_t* pixel = im;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			map_pixel(pixel);
			pixel += N_CHANNELS;
		}
	}

	char dst_path[65536];
	snprintf(dst_path, sizeof dst_path, "%s%s", dst_dir, src_path + strlen(src_dir));

	makedirs(dst_path);

	int ret;
	if (is_jpeg) {
		ret = stbi_write_jpg(dst_path, width, height, n_channels, im, 95);
	} else if (is_png) {
		ret = stbi_write_png(dst_path, width, height, n_channels, im, width*n_channels);
	} else {
		assert(!"UNREACHABLE");
	}
	if (ret == 0) {
		printf("%s: write failed\n", dst_path);
		stbi_image_free(im);
		return 0;
	}

	printf("[%s] => [%s]\n", src_path, dst_path);

	stbi_image_free(im);

	return 0;
}

static void process()
{
	const int nopenfd = 15;
	if (nftw(src_dir, visit, nopenfd, FTW_PHYS) == -1) {
		fprintf(stderr, "%s: nftw failed\n", src_dir);
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char** argv)
{
	palette_sanity_check();

	assert(argc > 0);
	prg = argv[0];

	if (argc <= 2) usage();

	char* cmd = argv[1];

	if (strcmp("prep", cmd) == 0) {
		if (argc != 4) usage();

		char* im_path = argv[2];

		int width, height, n_channels;
		uint8_t* im = stbi_load(im_path, &width, &height, &n_channels, N_CHANNELS);
		if (im == NULL) {
			fprintf(stderr, "%s: could not read\n", im_path);
			exit(EXIT_FAILURE);
		}

		#ifdef DEBUG
		printf("%s: %d×%d; %d channels\n", im_path, width, height, n_channels);
		#endif

		assert(n_channels == N_CHANNELS);

		if (width < PALETTE_WIDTH || height < PALETTE_HEIGHT) {
			fprintf(stderr, "%s is too small; it is only %d×%d; must be at least %d×%d\n", im_path, width, height, PALETTE_WIDTH, PALETTE_HEIGHT);
			exit(EXIT_FAILURE);
		}

		for (int y = 0; y < PALETTE_HEIGHT; y++) {
			for (int x = 0; x < PALETTE_WIDTH; x++) {
				uint8_t r,g,b;
				palette_xy_to_rgb(x, y, &r, &g, &b);
				uint8_t* pixel = &im[x*n_channels + y*width*n_channels];
				pixel[0] = r;
				pixel[1] = g;
				pixel[2] = b;
			}
		}

		char* image_out_path = argv[3];
		if (stbi_write_png(image_out_path, width, height, n_channels, im, width*n_channels) == 0) {
			fprintf(stderr, "%s: could not write\n", image_out_path);
			exit(EXIT_FAILURE);
		}

		stbi_image_free(im);
	} else if (strcmp("run", cmd) == 0) {
		if (argc != 5) usage();

		// construct look-up table (LUT) from reference image

		lut = calloc(LUT_N, sizeof *lut);
		assert(lut != NULL);

		{
			char* refim_path = argv[2];

			int width, height, n_channels;
			uint8_t* refim = stbi_load(refim_path, &width, &height, &n_channels, N_CHANNELS);
			assert(n_channels == N_CHANNELS);

			for (int y = 0; y < PALETTE_HEIGHT; y++) {
				for (int x = 0; x < PALETTE_WIDTH; x++) {
					uint8_t* pixel = &refim[n_channels * (x + y*width)];
					int luti = palette_xy_to_lut_index(x, y);
					uint8_t* lutp = &lut[luti];
					for (int i = 0; i < 3; i++) lutp[i] = pixel[i];
				}
			}

			stbi_image_free(refim);
		}

		{
			src_dir = strdup(argv[3]);
			assert(src_dir != NULL);
			sanitize_dir(src_dir);

			dst_dir = strdup(argv[4]);
			assert(dst_dir != NULL);
			sanitize_dir(dst_dir);

			process();
		}
	} else {
		fprintf(stderr, "invalid cmd %s\n", cmd);
	}

	return EXIT_SUCCESS;
}
