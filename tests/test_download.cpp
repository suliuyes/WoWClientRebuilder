/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2026 MaNGOS <https://www.getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

/**
 * @file test_download.cpp
 * @brief Unit tests for download_file(): local file:// round-trip and
 *        size-validation rejection.
 */

#include "doctest.h"
#include "download.h"
#include "bytes.h"
#include <string>
#include <algorithm>
static std::string file_url(const std::string& abs)
{
    std::string p = abs;
    std::replace(p.begin(), p.end(), '\\',
                 '/'); // Windows: convert backslashes for file:// URL
    return "file:///" + p;
}
TEST_CASE("download_file copies a file:// source and validates size")
{
    // arrange: write a source file
    std::string src = "dl_src.bin", dst = "dl_dst.bin";
    wcr::Bytes data(5000);
    for (size_t i = 0; i < data.size(); ++i)
    {
        data[i] = (uint8_t)(i * 7);
    }
    wcr::write_file(src, data);
    std::string abs = std::string(
        _fullpath(nullptr, src.c_str(), 0)); // absolute path for file://
    // act
    wcr::DownloadOpts o;
    o.expected_size = (long)data.size();
    o.resume = false;
    wcr::download_file(file_url(abs), dst, o);
    // assert
    CHECK(wcr::read_file(dst) == data);
    // wrong expected size should throw
    wcr::DownloadOpts bad;
    bad.expected_size = 1;
    bad.resume = false;
    CHECK_THROWS_AS(wcr::download_file(file_url(abs), dst, bad),
                    std::runtime_error);
    remove(src.c_str());
    remove(dst.c_str());
}

TEST_CASE("download_file skips the transfer when dest already has the "
          "expected size")
{
    // A .part that finished but was never renamed/recorded used to make every
    // later run fail: resume sent "Range: bytes=<size>-", the server answered
    // 416, and the run aborted.  An already-complete destination must be
    // recognized BEFORE any transfer, so an unreachable URL is never hit.
    std::string dst = "dl_complete.bin";
    wcr::Bytes seed(100, uint8_t('x'));
    wcr::write_file(dst, seed);
    wcr::DownloadOpts o;
    o.expected_size = 100;
    o.resume        = true;
    // Port 1 on loopback is unbound: any real connection attempt would throw.
    CHECK_NOTHROW(wcr::download_file("http://127.0.0.1:1/nope", dst, o));
    CHECK(wcr::read_file(dst) == seed); // untouched, not truncated
    remove(dst.c_str());
}

TEST_CASE("download_file restarts from zero when dest is larger than expected")
{
    // An oversized destination (e.g. a server once answered 200 to a Range
    // request and its full body was appended) can never be repaired by
    // appending; it must be truncated and re-downloaded, not resumed.
    std::string src = "dl_src100.bin", dst = "dl_oversize.bin";
    wcr::Bytes data(100);
    for (size_t i = 0; i < data.size(); ++i)
    {
        data[i] = (uint8_t)(i * 3);
    }
    wcr::write_file(src, data);
    wcr::Bytes junk(200, uint8_t('j')); // larger than expected
    wcr::write_file(dst, junk);
    std::string abs =
        std::string(_fullpath(nullptr, src.c_str(), 0));
    wcr::DownloadOpts o;
    o.expected_size = 100;
    o.resume        = true;
    wcr::download_file(file_url(abs), dst, o);
    CHECK(wcr::read_file(dst) == data); // exactly the source, no junk prefix
    remove(src.c_str());
    remove(dst.c_str());
}

TEST_CASE("download_file resumes a partial file:// source by appending")
{
    // Byte-level resume: dest holds the first 40 bytes of the source; the
    // transfer must fetch only the remainder and land byte-identical.
    std::string src = "dl_src_res.bin", dst = "dl_part_res.bin";
    wcr::Bytes data(100);
    for (size_t i = 0; i < data.size(); ++i)
    {
        data[i] = (uint8_t)(i * 11 + 5);
    }
    wcr::write_file(src, data);
    wcr::Bytes partial(data.begin(), data.begin() + 40);
    wcr::write_file(dst, partial);
    std::string abs =
        std::string(_fullpath(nullptr, src.c_str(), 0));
    wcr::DownloadOpts o;
    o.expected_size = 100;
    o.resume        = true;
    wcr::download_file(file_url(abs), dst, o);
    CHECK(wcr::read_file(dst) == data);
    remove(src.c_str());
    remove(dst.c_str());
}

TEST_CASE("download_file size mismatch throws SizeMismatchError")
{
    // The specific type lets reconstruct() discard the corrupt file instead
    // of leaving it to fail the same way on every later resume.
    std::string src = "dl_src_m.bin", dst = "dl_mismatch.bin";
    wcr::Bytes data(300);
    wcr::write_file(src, data);
    std::string abs = std::string(_fullpath(nullptr, src.c_str(), 0));
    wcr::DownloadOpts o;
    o.expected_size = 100; // wrong on purpose
    o.resume        = true;
    CHECK_THROWS_AS(wcr::download_file(file_url(abs), dst, o),
                    wcr::SizeMismatchError);
    remove(src.c_str());
    remove(dst.c_str());
}
