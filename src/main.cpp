/*
        Multiblend 2.0 (c) 2021 David Horman

        This program is free software: you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation, either version 3 of the License, or
        (at your option) any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program. If not, see <https://www.gnu.org/licenses/>.

        The author can be contacted at davidhorman47@gmail.com
*/

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

#include <jpeglib.h>
#include <tiffio.h>

#include "mb/file.h"
#include "mb/functions.h"
#include "mb/image.h"
#include "mb/jpeg.h"
#include "mb/linux_overrides.h"
#include "mb/mapalloc.h"
#include "mb/multiblend.h"
#include "mb/pnger.h"
#include "mb/tiff.h"

namespace mb = multiblend;
namespace utils = mb::utils;
namespace io = mb::io;
namespace memory = mb::memory;

void run_main(int argc, char* argv[]) {
  mb::utils::Timer timer_all;
  mb::utils::Timer timer;
  timer_all.Start();

  TIFFSetWarningHandler(nullptr);

  /***********************************************************************
   * Variables
   ***********************************************************************/
  std::vector<io::Image> images;
  int fixed_levels = 0;
  int add_levels = 0;

  bool no_mask = false;
  bool big_tiff = false;
  bool bgr = false;
  bool wideblend = false;
  bool reverse = false;
  bool timing = false;
  bool dither = true;
  bool gamma = false;
  bool all_threads = true;
  int wrap = 0;

  io::tiff::TiffPtr tiff_file = nullptr;
  io::FilePtr jpeg_file = nullptr;
  std::optional<io::png::Pnger> png_file;
  io::ImageType output_type = io::ImageType::MB_NONE;
  int jpeg_quality = -1;
  int compression = -1;
  char* seamsave_filename = nullptr;
  char* seamload_filename = nullptr;
  char* xor_filename = nullptr;
  char* output_filename = nullptr;
  int output_bpp = 0;

  double write_time = 0;

  /***********************************************************************
   * Help
   ***********************************************************************/
  if (argc == 1 || (strcmp(argv[1], "-h") == 0) ||
      (strcmp(argv[1], "--help") == 0) || (strcmp(argv[1], "/?") == 0)) {
    utils::Output(1, "\n");
    utils::Output(1,
                  "Multiblend v2.0.0 (c) 2021 David Horman        "
                  "http://horman.net/multiblend/\n");
    utils::Output(
        1,
        "-------------------------------------------------------------------"
        "---------\n");

    printf(
        "Usage: multiblend [options] [-o OUTPUT] INPUT [X,Y] [INPUT] [X,Y] "
        "[INPUT]...\n");
    printf("\n");
    printf("Options:\n");
    printf("  --levels X / -l X      X: set number of blending levels to X\n");
    printf(
        "                        -X: decrease number of blending levels by "
        "X\n");
    printf(
        "                        +X: increase number of blending levels by "
        "X\n");
    printf(
        "  --depth D / -d D       Override automatic output image depth (8 or "
        "16)\n");
    printf("  --bgr                  Swap RGB order\n");
    printf(
        "  --wideblend            Calculate number of levels based on output "
        "image size,\n");
    printf("                         rather than input image size\n");
    printf(
        "  -w, --wrap=[mode]      Blend around images boundaries (NONE "
        "(default),\n");
    printf(
        "                         HORIZONTAL, VERTICAL). When specified "
        "without a mode,\n");
    printf("                         defaults to HORIZONTAL.\n");
    printf(
        "  --compression=X        Output file compression. For TIFF output, X "
        "may be:\n");
    printf("                         NONE (default), PACKBITS, or LZW\n");
    printf(
        "                         For JPEG output, X is JPEG quality (0-100, "
        "default 75)\n");
    printf(
        "                         For PNG output, X is PNG filter (0-9, "
        "default 3)\n");
    printf(
        "  --cache-threshold=     Allocate memory beyond X "
        "bytes/[K]ilobytes/\n");
    printf("      X[K/M/G]           [M]egabytes/[G]igabytes to disk\n");
    printf("  --no-dither            Disable dithering\n");
    printf(
        "  --tempdir <dir>        Specify temporary directory (default: system "
        "temp)\n");
    printf(
        "  --save-seams <file>    Save seams to PNG file for external "
        "editing\n");
    printf("  --load-seams <file>    Load seams from PNG file\n");
    printf(
        "  --no-output            Do not blend (for use with --save-seams)\n");
    printf(
        "                         Must be specified as last option before "
        "input images\n");
    printf("  --bigtiff              BigTIFF output\n");
    printf(
        "  --reverse              Reverse image priority (last=highest) for "
        "resolving\n");
    printf("                         indeterminate pixels\n");
    printf("  --quiet                Suppress output (except warnings)\n");
    printf("  --all-threads          Use all available CPU threads\n");
    printf(
        "  [X,Y]                  Optional position adjustment for previous "
        "input image\n");
    exit(EXIT_SUCCESS);
  }

  /***********************************************************************
  ************************************************************************
  * Parse arguments
  ************************************************************************
  ***********************************************************************/
  std::vector<char*> my_argv;

  bool skip = false;

  for (int i = 1; i < argc; ++i) {
    my_argv.push_back(argv[i]);

    if (!skip) {
      int c = 0;

      while (argv[i][c] != 0) {
        if (argv[i][c] == '=') {
          argv[i][c++] = 0;
          if (argv[i][c] != 0) {
            my_argv.push_back(&argv[i][c]);
          }
          break;
        }
        ++c;
      }

      if ((strcmp(argv[i], "-o") == 0) || (strcmp(argv[i], "--output") == 0)) {
        skip = true;
      }
    }
  }

  if ((int)my_argv.size() < 3) {
    utils::die_throw("Error: Not enough arguments (try -h for help)");
  }

  int pos;
  for (pos = 0; pos < (int)my_argv.size(); ++pos) {
    if ((strcmp(my_argv[pos], "-d") == 0) ||
        (strcmp(my_argv[pos], "--d") == 0) ||
        (strcmp(my_argv[pos], "--depth") == 0) ||
        (strcmp(my_argv[pos], "--bpp") == 0)) {
      if (++pos < (int)my_argv.size()) {
        output_bpp = atoi(my_argv[pos]);
        if (output_bpp != 8 && output_bpp != 16) {
          utils::die_throw("Error: Invalid output depth specified");
        }
      } else {
        utils::die_throw("Error: Missing parameter value");
      }
    } else if ((strcmp(my_argv[pos], "-l") == 0) ||
               (strcmp(my_argv[pos], "--levels") == 0)) {
      if (++pos < (int)my_argv.size()) {
        int n;
        if (my_argv[pos][0] == '+' || my_argv[pos][0] == '-') {
          sscanf_s(my_argv[pos], "%d%n", &add_levels, &n);
        } else {
          sscanf_s(my_argv[pos], "%d%n", &fixed_levels, &n);
          if (fixed_levels == 0) {
            fixed_levels = 1;
          }
        }
        if (my_argv[pos][n] != 0) {
          utils::die_throw("Error: Bad --levels parameter");
        }
      } else {
        utils::die_throw("Error: Missing parameter value");
      }
    } else if ((strcmp(my_argv[pos], "--wrap") == 0) ||
               (strcmp(my_argv[pos], "-w") == 0)) {
      if (pos + 1 >= (int)my_argv.size()) {
        utils::die_throw("Error: Missing parameters");
      }
      if ((strcmp(my_argv[pos + 1], "none") == 0) ||
          (strcmp(my_argv[pos + 1], "open") == 0)) {
        ++pos;
      } else if ((strcmp(my_argv[pos + 1], "horizontal") == 0) ||
                 (strcmp(my_argv[pos + 1], "h") == 0)) {
        wrap = 1;
        ++pos;
      } else if ((strcmp(my_argv[pos + 1], "vertical") == 0) ||
                 (strcmp(my_argv[pos + 1], "v") == 0)) {
        wrap = 2;
        ++pos;
      } else if ((strcmp(my_argv[pos + 1], "both") == 0) ||
                 (strcmp(my_argv[pos + 1], "hv") == 0)) {
        wrap = 3;
        ++pos;
      } else {
        wrap = 1;
      }
    } else if (strcmp(my_argv[pos], "--cache-threshold") == 0) {
      if (pos + 1 >= (int)my_argv.size()) {
        utils::die_throw("Error: Missing parameters");
      }
      ++pos;
      int shift = 0;
      int n = 0;
      std::size_t len = strlen(my_argv[pos]);
      std::size_t threshold;
      sscanf_s(my_argv[pos], "%zu%n", &threshold, &n);
      if (n != len) {
        if (n == len - 1) {
          switch (my_argv[pos][len - 1]) {
            case 'k':
            case 'K':
              shift = 10;
              break;
            case 'm':
            case 'M':
              shift = 20;
              break;
            case 'g':
            case 'G':
              shift = 30;
              break;
            default:
              utils::die_throw("Error: Bad --cache-threshold parameter");
          }
          threshold <<= shift;
        } else {
          utils::die_throw("Error: Bad --cache-threshold parameter");
        }
      }
      memory::MapAlloc::CacheThreshold(threshold);
    } else if ((strcmp(my_argv[pos], "--nomask") == 0) ||
               (strcmp(my_argv[pos], "--no-mask") == 0)) {
      no_mask = true;
    } else if ((strcmp(my_argv[pos], "--timing") == 0) ||
               (strcmp(my_argv[pos], "--timings") == 0)) {
      timing = true;
    } else if (strcmp(my_argv[pos], "--bigtiff") == 0) {
      big_tiff = true;
    } else if (strcmp(my_argv[pos], "--bgr") == 0) {
      bgr = true;
    } else if (strcmp(my_argv[pos], "--wideblend") == 0) {
      wideblend = true;
    } else if (strcmp(my_argv[pos], "--reverse") == 0) {
      reverse = true;
    } else if (strcmp(my_argv[pos], "--gamma") == 0) {
      gamma = true;
    } else if ((strcmp(my_argv[pos], "--no-dither") == 0) ||
               (strcmp(my_argv[pos], "--nodither") == 0)) {
      dither = false;
      //  else if (!strcmp(my_argv[i], "--force"))     force_coverage =
      // true;
    } else if (strncmp(my_argv[pos], "-f", 2) == 0) {
      utils::Output(0, "ignoring Enblend option -f\n");
    } else if (strcmp(my_argv[pos], "-a") == 0) {
      utils::Output(0, "ignoring Enblend option -a\n");
    } else if (strcmp(my_argv[pos], "--no-ciecam") == 0) {
      utils::Output(0, "ignoring Enblend option --no-ciecam\n");
    } else if (strcmp(my_argv[pos], "--primary-seam-generator") == 0) {
      utils::Output(0, "ignoring Enblend option --primary-seam-generator\n");
      ++pos;
    }

    else if (strcmp(my_argv[pos], "--compression") == 0) {
      if (++pos < (int)my_argv.size()) {
        if (strcmp(my_argv[pos], "0") == 0) {
          jpeg_quality = 0;
        } else if (atoi(my_argv[pos]) > 0) {
          jpeg_quality = atoi(my_argv[pos]);
        } else if (_stricmp(my_argv[pos], "lzw") == 0) {
          compression = COMPRESSION_LZW;
        } else if (_stricmp(my_argv[pos], "packbits") == 0) {
          compression = COMPRESSION_PACKBITS;
          //    else if (_stricmp(my_argv[i], "deflate")
          //== 0) compression = COMPRESSION_DEFLATE;
        } else if (_stricmp(my_argv[pos], "none") == 0) {
          compression = COMPRESSION_NONE;
        } else {
          utils::die_throw("Error: Unknown compression codec {}", my_argv[pos]);
        }
      } else {
        utils::die_throw("Error: Missing parameter value");
      }
    } else if ((strcmp(my_argv[pos], "-v") == 0) ||
               (strcmp(my_argv[pos], "--verbose") == 0)) {
      ++utils::verbosity;
    } else if ((strcmp(my_argv[pos], "-q") == 0) ||
               (strcmp(my_argv[pos], "--quiet") == 0)) {
      --utils::verbosity;
    } else if (((strcmp(my_argv[pos], "--saveseams") == 0) ||
                (strcmp(my_argv[pos], "--save-seams") == 0)) &&
               pos < (int)my_argv.size() - 1) {
      seamsave_filename = my_argv[++pos];
    } else if (((strcmp(my_argv[pos], "--loadseams") == 0) ||
                (strcmp(my_argv[pos], "--load-seams") == 0)) &&
               pos < (int)my_argv.size() - 1) {
      seamload_filename = my_argv[++pos];
    } else if (((strcmp(my_argv[pos], "--savexor") == 0) ||
                (strcmp(my_argv[pos], "--save-xor") == 0)) &&
               pos < (int)my_argv.size() - 1) {
      xor_filename = my_argv[++pos];
    } else if ((strcmp(my_argv[pos], "--tempdir") == 0) ||
               (strcmp(my_argv[pos], "--tmpdir") == 0) &&
                   pos < (int)my_argv.size() - 1) {
      memory::MapAlloc::SetTmpdir(my_argv[++pos]);
    } else if (strcmp(my_argv[pos], "--all-threads") == 0) {
      all_threads = true;
    } else if ((strcmp(my_argv[pos], "-o") == 0) ||
               (strcmp(my_argv[pos], "--output") == 0)) {
      if (++pos < (int)my_argv.size()) {
        output_filename = my_argv[pos];
        char* ext = strrchr(output_filename, '.');

        if (ext == nullptr) {
          utils::die_throw("Error: Unknown output filetype");
        }

        ++ext;
        if (!((_stricmp(ext, "jpg") != 0) && (_stricmp(ext, "jpeg") != 0))) {
          output_type = io::ImageType::MB_JPEG;
          if (jpeg_quality == -1) {
            jpeg_quality = 75;
          }
        } else if (!((_stricmp(ext, "tif") != 0) &&
                     (_stricmp(ext, "tiff") != 0))) {
          output_type = io::ImageType::MB_TIFF;
        } else if (_stricmp(ext, "png") == 0) {
          output_type = io::ImageType::MB_PNG;
        } else {
          utils::die_throw("Error: Unknown file extension");
        }

        ++pos;
        break;
      }
    } else if (strcmp(my_argv[pos], "--no-output") == 0) {
      ++pos;
      break;
    } else {
      utils::die_throw("Error: Unknown argument \"{}\"", my_argv[pos]);
    }
  }

  if (compression != -1) {
    if (output_type != io::ImageType::MB_TIFF) {
      utils::Output(
          0, "Warning: non-TIFF output; ignoring TIFF compression setting\n");
    }
  } else if (output_type == io::ImageType::MB_TIFF) {
    compression = COMPRESSION_LZW;
  }

  if (jpeg_quality != -1 && output_type != io::ImageType::MB_JPEG &&
      output_type != io::ImageType::MB_PNG) {
    utils::Output(0,
                  "Warning: non-JPEG/PNG output; ignoring compression quality "
                  "setting\n");
  }

  if ((jpeg_quality < -1 || jpeg_quality > 9) &&
      output_type == io::ImageType::MB_PNG) {
    utils::die_throw("Error: Bad PNG compression quality setting\n");
  }

  if (output_type == io::ImageType::MB_NONE && (seamsave_filename == nullptr)) {
    utils::die_throw("Error: No output file specified");
  }
  if ((seamload_filename != nullptr) && (seamsave_filename != nullptr)) {
    utils::die_throw("Error: Cannot load and save seams at the same time");
  }
  if (wrap == 3) {
    utils::die_throw(
        "Error: Wrapping in both directions is not currently supported");
  }

  if (strcmp(my_argv[pos], "--") == 0) {
    ++pos;
  }

  /***********************************************************************
   * Push remaining arguments to images vector
   ***********************************************************************/

  while (pos < (int)my_argv.size()) {
    if (!images.empty()) {
      int x;
      int y;
      int n = 0;
      sscanf_s(my_argv[pos], "%d,%d%n", &x, &y, &n);
      if (my_argv[pos][n] == 0) {
        images.back().xpos_add_ = x;
        images.back().ypos_add_ = y;
        pos++;
        continue;
      }
    }
    images.emplace_back(my_argv[pos++]);
  }

  int n_images = (int)images.size();

  if (n_images == 0) {
    utils::die_throw("Error: No input files specified");
  }
  if ((seamsave_filename != nullptr) && n_images > 256) {
    seamsave_filename = nullptr;
    utils::Output(
        0, "Warning: seam saving not possible with more than 256 images");
  }
  if ((seamload_filename != nullptr) && n_images > 256) {
    seamload_filename = nullptr;
    utils::Output(
        0, "Warning: seam loading not possible with more than 256 images");
  }
  if ((xor_filename != nullptr) && n_images > 255) {
    xor_filename = nullptr;
    utils::Output(
        0, "Warning: XOR map saving not possible with more than 255 images");
  }

  /***********************************************************************
   * Print banner
   ***********************************************************************/
  utils::Output(1, "\n");
  utils::Output(1,
                "Multiblend v2.0.0 (c) 2021 David Horman        "
                "http://horman.net/multiblend/\n");
  utils::Output(
      1,
      "---------------------------------------------------------------------"
      "-------\n");

  /***********************************************************************
  ************************************************************************
  * Open output
  ************************************************************************
  ***********************************************************************/
  switch (output_type) {
    case io::ImageType::MB_TIFF: {
      if (!big_tiff) {
        tiff_file = {TIFFOpen(output_filename, "w"), io::tiff::CloseDeleter{}};
      } else {
        tiff_file = {TIFFOpen(output_filename, "w8"), io::tiff::CloseDeleter{}};
      }
      if (tiff_file == nullptr) {
        utils::die_throw("Error: Could not open output file");
      }
    } break;
    case io::ImageType::MB_JPEG: {
      if (output_bpp == 16) {
        utils::die_throw(
            "Error: 16bpp output is incompatible with JPEG output");
      }
    }
      [[fallthrough]];
    case io::ImageType::MB_PNG: {
      FILE* tmp_jpeg_file = nullptr;
      fopen_s(&tmp_jpeg_file, output_filename, "wb");
      if (tmp_jpeg_file == nullptr) {
        utils::die_throw("Error: Could not open output file");
      }
      jpeg_file = {tmp_jpeg_file, io::FileDeleter{}};
    } break;
  }

  /***********************************************************************
  ************************************************************************
  * Process images
  ************************************************************************
  ***********************************************************************/
  auto result =
      mb::Multiblend(images, {
                                 .output_type = output_type,
                                 .output_bpp = output_bpp,
                                 .fixed_levels = fixed_levels,
                                 .wideblend = wideblend,
                                 .add_levels = add_levels,
                                 .all_threads = all_threads,
                                 .reverse = reverse,
                                 .wrap = wrap,
                                 .dither = dither,
                                 .gamma = gamma,
                                 .no_mask = no_mask,
                                 .seamsave_filename = seamsave_filename,
                                 .seamload_filename = seamload_filename,
                                 .xor_filename = xor_filename,
                             });

  /***********************************************************************
   * Write
   ***********************************************************************/
  constexpr int rows_per_strip = 64;

  if (output_type != io::ImageType::MB_NONE) {
    utils::Output(1, "Writing %s...\n", output_filename);

    timer.Start();

    std::unique_ptr<jpeg_error_mgr> jerr;
    std::unique_ptr<jpeg_compress_struct, io::jpeg::CompressDeleter> cinfo;

    // JSAMPARRAY scanlines = nullptr;
    std::unique_ptr<JSAMPROW[]> scanlines = nullptr;

    int spp = result.no_mask ? 3 : 4;

    int bytes_per_pixel = spp << (result.output_bpp >> 4);
    int bytes_per_row = bytes_per_pixel * result.width;

    int n_strips = (int)((result.height + rows_per_strip - 1) / rows_per_strip);
    int remaining = result.height;
    auto strip = std::make_unique<uint8_t[]>(
        (rows_per_strip * (int64_t)result.width) * bytes_per_pixel);
    void* oc_p[3] = {result.output_channels[0].get(),
                     result.output_channels[1].get(),
                     result.output_channels[2].get()};
    if (bgr) {
      std::swap(oc_p[0], oc_p[2]);
    }

    switch (output_type) {
      case io::ImageType::MB_TIFF: {
        TIFFSetField(tiff_file.get(), TIFFTAG_IMAGEWIDTH, result.width);
        TIFFSetField(tiff_file.get(), TIFFTAG_IMAGELENGTH, result.height);
        TIFFSetField(tiff_file.get(), TIFFTAG_COMPRESSION, compression);
        TIFFSetField(tiff_file.get(), TIFFTAG_PLANARCONFIG,
                     PLANARCONFIG_CONTIG);
        TIFFSetField(tiff_file.get(), TIFFTAG_ROWSPERSTRIP, rows_per_strip);
        TIFFSetField(tiff_file.get(), TIFFTAG_BITSPERSAMPLE, result.output_bpp);
        if (result.no_mask) {
          TIFFSetField(tiff_file.get(), TIFFTAG_SAMPLESPERPIXEL, 3);
        } else {
          TIFFSetField(tiff_file.get(), TIFFTAG_SAMPLESPERPIXEL, 4);
          uint16_t out[1] = {EXTRASAMPLE_UNASSALPHA};
          TIFFSetField(tiff_file.get(), TIFFTAG_EXTRASAMPLES, 1, &out);
        }

        TIFFSetField(tiff_file.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        if (images[0].tiff_xres_ != -1) {
          TIFFSetField(tiff_file.get(), TIFFTAG_XRESOLUTION,
                       images[0].tiff_xres_);
          TIFFSetField(tiff_file.get(), TIFFTAG_XPOSITION,
                       (float)(result.min_xpos / images[0].tiff_xres_));
        }
        if (images[0].tiff_yres_ != -1) {
          TIFFSetField(tiff_file.get(), TIFFTAG_YRESOLUTION,
                       images[0].tiff_yres_);
          TIFFSetField(tiff_file.get(), TIFFTAG_YPOSITION,
                       (float)(result.min_ypos / images[0].tiff_yres_));
        }

        if (images[0].geotiff_.set) {
          // if we got a georeferenced input, store the geotags in the output
          io::tiff::GeoTIFFInfo info(images[0].geotiff_);
          info.XGeoRef = result.min_xpos * images[0].geotiff_.XCellRes;
          info.YGeoRef = -result.min_ypos * images[0].geotiff_.YCellRes;
          utils::Output(1, "Output georef: UL: %f %f, pixel size: %f %f\n",
                        info.XGeoRef, info.YGeoRef, info.XCellRes,
                        info.YCellRes);
          io::tiff::geotiff_write(tiff_file.get(), &info);
        }
      } break;
      case io::ImageType::MB_JPEG: {
        cinfo = {new jpeg_compress_struct{}, io::jpeg::CompressDeleter{}};
        jerr = std::make_unique<jpeg_error_mgr>();

        cinfo->err = jpeg_std_error(jerr.get());
        jpeg_create_compress(cinfo.get());
        jpeg_stdio_dest(cinfo.get(), jpeg_file.get());

        cinfo->image_width = result.width;
        cinfo->image_height = result.height;
        cinfo->input_components = 3;
        cinfo->in_color_space = JCS_RGB;

        jpeg_set_defaults(cinfo.get());
        jpeg_set_quality(cinfo.get(), jpeg_quality, 1);
        jpeg_start_compress(cinfo.get(), 1);
      } break;
      case io::ImageType::MB_PNG: {
        png_file = io::png::Pnger(
            output_filename, nullptr, result.width, result.height,
            result.no_mask ? io::png::ColorType::RGB
                           : io::png::ColorType::RGB_ALPHA,
            result.output_bpp, std::move(jpeg_file), jpeg_quality);
      } break;
    }

    if (output_type == io::ImageType::MB_PNG ||
        output_type == io::ImageType::MB_JPEG) {
      scanlines = std::make_unique<JSAMPROW[]>(rows_per_strip);
      for (int i = 0; i < rows_per_strip; ++i) {
        scanlines[i] = (JSAMPROW) &
                       (strip.get())[static_cast<ptrdiff_t>(i) * bytes_per_row];
      }
    }

    result.full_mask.Start();

    for (int s = 0; s < n_strips; ++s) {
      int strip_p = 0;
      int rows = std::min(remaining, rows_per_strip);

      for (int strip_y = 0; strip_y < rows; ++strip_y) {
        int x = 0;
        while (x < result.width) {
          uint32_t cur = result.full_mask.ReadForwards32();
          if ((cur & 0x80000000) != 0u) {
            int lim = x + (cur & 0x7fffffff);
            switch (result.output_bpp) {
              case 8: {
                while (x < lim) {
                  (strip.get())[strip_p++] = ((uint8_t*)(oc_p[0]))[x];
                  (strip.get())[strip_p++] = ((uint8_t*)(oc_p[1]))[x];
                  (strip.get())[strip_p++] = ((uint8_t*)(oc_p[2]))[x];
                  if (!result.no_mask) {
                    (strip.get())[strip_p++] = 0xff;
                  }
                  ++x;
                }
              } break;
              case 16: {
                while (x < lim) {
                  ((uint16_t*)strip.get())[strip_p++] =
                      ((uint16_t*)(oc_p[0]))[x];
                  ((uint16_t*)strip.get())[strip_p++] =
                      ((uint16_t*)(oc_p[1]))[x];
                  ((uint16_t*)strip.get())[strip_p++] =
                      ((uint16_t*)(oc_p[2]))[x];
                  if (!result.no_mask) {
                    ((uint16_t*)strip.get())[strip_p++] = 0xffff;
                  }
                  ++x;
                }
              } break;
            }
          } else {
            std::size_t t = (std::size_t)cur * bytes_per_pixel;
            switch (result.output_bpp) {
              case 8: {
                ZeroMemory(&(strip.get())[strip_p], t);
              } break;
              case 16: {
                ZeroMemory(&((uint16_t*)strip.get())[strip_p], t);
              } break;
            }
            strip_p += cur * spp;
            x += cur;
          }
        }

        switch (result.output_bpp) {
          case 8: {
            oc_p[0] = &((uint8_t*)(oc_p[0]))[result.width];
            oc_p[1] = &((uint8_t*)(oc_p[1]))[result.width];
            oc_p[2] = &((uint8_t*)(oc_p[2]))[result.width];
          } break;
          case 16: {
            oc_p[0] = &((uint16_t*)(oc_p[0]))[result.width];
            oc_p[1] = &((uint16_t*)(oc_p[1]))[result.width];
            oc_p[2] = &((uint16_t*)(oc_p[2]))[result.width];
          } break;
        }
      }

      switch (output_type) {
        case io::ImageType::MB_TIFF: {
          TIFFWriteEncodedStrip(tiff_file.get(), s, strip.get(),
                                rows * (int64_t)bytes_per_row);
        } break;
        case io::ImageType::MB_JPEG: {
          jpeg_write_scanlines(cinfo.get(), scanlines.get(), rows);
        } break;
        case io::ImageType::MB_PNG: {
          png_file->WriteRows(scanlines.get(), rows);
        } break;
      }

      remaining -= rows_per_strip;
    }

    write_time = timer.Read();
  }

  /***********************************************************************
   * Timing
   ***********************************************************************/
  if (timing) {
    printf("\n");
    printf("Images:   %.3fs\n", result.timing.images_time);
    printf("Seaming:  %.3fs\n", result.timing.seam_time);
    if (output_type != io::ImageType::MB_NONE) {
      printf("Masks:    %.3fs\n", result.timing.shrink_mask_time);
      printf("Copy:     %.3fs\n", result.timing.copy_time);
      printf("Shrink:   %.3fs\n", result.timing.shrink_time);
      printf("Laplace:  %.3fs\n", result.timing.laplace_time);
      printf("Blend:    %.3fs\n", result.timing.blend_time);
      printf("Collapse: %.3fs\n", result.timing.collapse_time);
      if (wrap != 0) {
        printf("Wrapping: %.3fs\n", result.timing.wrap_time);
      }
      printf("Output:   %.3fs\n", result.timing.out_time);
      printf("Write:    %.3fs\n", write_time);
    }
  }

  /***********************************************************************
   * Clean up
   ***********************************************************************/
  if (timing) {
    if (output_type == io::ImageType::MB_NONE) {
      timer_all.Report("\nExecution complete. Total execution time");
    } else {
      timer_all.Report("\nBlend complete. Total execution time");
    }
  }
}

int main(int argc, char* argv[]) {
  try {
    run_main(argc, argv);
  } catch (const std::exception& e) {
    fprintf(stderr, "Error: %s\n", e.what());
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
