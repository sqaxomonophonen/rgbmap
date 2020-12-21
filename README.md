Quick'n'dirty color correction batch tool. It's a two-step process that works
by burning-in a "known" color palette into a reference image. You then color
correct that, and apply the transform to a batch of images.

Problems:
 - doesn't copy metadata (could possibly be handled using `exiftool -TagsFromFile`)
 - doesn't copy file date
 - single-threaded / not parallelized
