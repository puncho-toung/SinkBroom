/*
 Copyright (C) 2026 TrenchBroom contributors

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

// LP1c.2 (D27, M-IMG-BUILD-01): the engine-vendored tinyexr.h embeds miniz v1.15 inside
// namespace tinyexr::miniz, but declares its functions extern "C", so their linker names
// are miniz's own and clash with the miniz TrenchBroom links (TbFsLib's zip reader).
// These macros give every external function of the embedded copy a name of its own before
// the copied Image_files.cpp includes tinyexr.h (shim/src/ShimImageFiles.cpp). Only the
// names change; the code is the engine's. The mz_zip_* functions are compiled out
// (MINIZ_NO_ARCHIVE_APIS) and renamed all the same, so the list is the header's.

// clang-format off: one name per line, as tinyexr.h declares them
#define mz_adler32 tbLitExr_mz_adler32
#define mz_compress tbLitExr_mz_compress
#define mz_compress2 tbLitExr_mz_compress2
#define mz_compressBound tbLitExr_mz_compressBound
#define mz_crc32 tbLitExr_mz_crc32
#define mz_deflate tbLitExr_mz_deflate
#define mz_deflateBound tbLitExr_mz_deflateBound
#define mz_deflateEnd tbLitExr_mz_deflateEnd
#define mz_deflateInit tbLitExr_mz_deflateInit
#define mz_deflateInit2 tbLitExr_mz_deflateInit2
#define mz_deflateReset tbLitExr_mz_deflateReset
#define mz_error tbLitExr_mz_error
#define mz_free tbLitExr_mz_free
#define mz_inflate tbLitExr_mz_inflate
#define mz_inflateEnd tbLitExr_mz_inflateEnd
#define mz_inflateInit tbLitExr_mz_inflateInit
#define mz_inflateInit2 tbLitExr_mz_inflateInit2
#define mz_uncompress tbLitExr_mz_uncompress
#define mz_version tbLitExr_mz_version
#define mz_zip_add_mem_to_archive_file_in_place tbLitExr_mz_zip_add_mem_to_archive_file_in_place
#define mz_zip_extract_archive_file_to_heap tbLitExr_mz_zip_extract_archive_file_to_heap
#define mz_zip_reader_end tbLitExr_mz_zip_reader_end
#define mz_zip_reader_extract_file_to_callback tbLitExr_mz_zip_reader_extract_file_to_callback
#define mz_zip_reader_extract_file_to_file tbLitExr_mz_zip_reader_extract_file_to_file
#define mz_zip_reader_extract_file_to_heap tbLitExr_mz_zip_reader_extract_file_to_heap
#define mz_zip_reader_extract_file_to_mem tbLitExr_mz_zip_reader_extract_file_to_mem
#define mz_zip_reader_extract_file_to_mem_no_alloc tbLitExr_mz_zip_reader_extract_file_to_mem_no_alloc
#define mz_zip_reader_extract_to_callback tbLitExr_mz_zip_reader_extract_to_callback
#define mz_zip_reader_extract_to_file tbLitExr_mz_zip_reader_extract_to_file
#define mz_zip_reader_extract_to_heap tbLitExr_mz_zip_reader_extract_to_heap
#define mz_zip_reader_extract_to_mem tbLitExr_mz_zip_reader_extract_to_mem
#define mz_zip_reader_extract_to_mem_no_alloc tbLitExr_mz_zip_reader_extract_to_mem_no_alloc
#define mz_zip_reader_file_stat tbLitExr_mz_zip_reader_file_stat
#define mz_zip_reader_get_filename tbLitExr_mz_zip_reader_get_filename
#define mz_zip_reader_get_num_files tbLitExr_mz_zip_reader_get_num_files
#define mz_zip_reader_init tbLitExr_mz_zip_reader_init
#define mz_zip_reader_init_file tbLitExr_mz_zip_reader_init_file
#define mz_zip_reader_init_mem tbLitExr_mz_zip_reader_init_mem
#define mz_zip_reader_is_file_a_directory tbLitExr_mz_zip_reader_is_file_a_directory
#define mz_zip_reader_is_file_encrypted tbLitExr_mz_zip_reader_is_file_encrypted
#define mz_zip_reader_locate_file tbLitExr_mz_zip_reader_locate_file
#define mz_zip_writer_add_file tbLitExr_mz_zip_writer_add_file
#define mz_zip_writer_add_from_zip_reader tbLitExr_mz_zip_writer_add_from_zip_reader
#define mz_zip_writer_add_mem tbLitExr_mz_zip_writer_add_mem
#define mz_zip_writer_add_mem_ex tbLitExr_mz_zip_writer_add_mem_ex
#define mz_zip_writer_end tbLitExr_mz_zip_writer_end
#define mz_zip_writer_finalize_archive tbLitExr_mz_zip_writer_finalize_archive
#define mz_zip_writer_finalize_heap_archive tbLitExr_mz_zip_writer_finalize_heap_archive
#define mz_zip_writer_init tbLitExr_mz_zip_writer_init
#define mz_zip_writer_init_file tbLitExr_mz_zip_writer_init_file
#define mz_zip_writer_init_from_reader tbLitExr_mz_zip_writer_init_from_reader
#define mz_zip_writer_init_heap tbLitExr_mz_zip_writer_init_heap
#define tdefl_compress tbLitExr_tdefl_compress
#define tdefl_compress_buffer tbLitExr_tdefl_compress_buffer
#define tdefl_compress_mem_to_heap tbLitExr_tdefl_compress_mem_to_heap
#define tdefl_compress_mem_to_mem tbLitExr_tdefl_compress_mem_to_mem
#define tdefl_compress_mem_to_output tbLitExr_tdefl_compress_mem_to_output
#define tdefl_create_comp_flags_from_zip_params tbLitExr_tdefl_create_comp_flags_from_zip_params
#define tdefl_get_adler32 tbLitExr_tdefl_get_adler32
#define tdefl_get_prev_return_status tbLitExr_tdefl_get_prev_return_status
#define tdefl_init tbLitExr_tdefl_init
#define tdefl_write_image_to_png_file_in_memory tbLitExr_tdefl_write_image_to_png_file_in_memory
#define tdefl_write_image_to_png_file_in_memory_ex tbLitExr_tdefl_write_image_to_png_file_in_memory_ex
#define tinfl_decompress tbLitExr_tinfl_decompress
#define tinfl_decompress_mem_to_callback tbLitExr_tinfl_decompress_mem_to_callback
#define tinfl_decompress_mem_to_heap tbLitExr_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem tbLitExr_tinfl_decompress_mem_to_mem
// clang-format on
