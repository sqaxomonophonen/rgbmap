Quick'n'dirty color correction batch tool. It's a two-step process that works
by burning-in a "known" color palette into a reference image. You then color
correct this image, say in GIMP, and then apply the transform to a batch of
images.

Problems:
 - Doesn't copy metadata (could possibly be handled using `exiftool -TagsFromFile`)
 - Doesn't copy file date
 - Single-threaded / not parallelized
 - Performance? Trilinear interpolation might _actually_ be slower than cache
   misses in a 1:1 256×256×256×3 LUT (48MB)... let's find out?
