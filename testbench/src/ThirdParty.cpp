// The single translation unit that compiles miniaudio and stb_image.
//
// Both are single-header libraries with an implementation macro; keeping them
// here stops the rest of the testbench recompiling four megabytes of miniaudio
// every time a slider moves.

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"
