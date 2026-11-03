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
 * @file fetch.cpp
 * @brief Source-dispatching fetch engine implementation.
 */

#include "fetch.h"
#include "download.h"
#include "zip.h"
#include "mpq.h"
#include "ptch.h"
#include "md5.h"
#include "journal.h"
#include "pieces.h"
#include "progress.h"
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace wcr
{
std::string repair_url(const std::string& base, const std::string& md5)
{
    if (md5.size() < 2)
    {
        throw std::runtime_error("repair_url: md5 too short: " + md5);
    }
    return base + "/" + md5.substr(0, 1) + "/" + md5.substr(1, 1) + "/" + md5;
}

std::string swap_base(const std::string& url, const std::string& from,
                      const std::string& to)
{
    std::string::size_type pos = url.find(from);
    if (pos == std::string::npos)
    {
        return url;
    }
    return url.substr(0, pos) + to + url.substr(pos + from.size());
}

std::string swap_region(const std::string& url, const std::string& toSeg)
{
    static const std::string kEu = "/EU/";
    static const std::string kNa = "/NA/";
    const std::string anchor = "wow-pod-retail";
    std::string::size_type a = url.find(anchor);
    if (a == std::string::npos)
    {
        return url;
    }
    std::string::size_type eu = url.find(kEu, a);
    std::string::size_type na = url.find(kNa, a);
    if (eu != std::string::npos)
    {
        return url.substr(0, eu) + toSeg + url.substr(eu + kEu.size());
    }
    if (na != std::string::npos)
    {
        return url.substr(0, na) + toSeg + url.substr(na + kNa.size());
    }
    return url;
}

std::string region_segment(const std::string& region)
{
    return (region == "NA") ? "/NA/" : "/EU/";
}

void apply_region_to_recipe(Recipe& run, const Recipe& base,
                            const std::string& region)
{
    for (MpqSource& m : run.mpqs)
    {
        m.url = swap_region(m.url, region_segment(region));
        // Some archives are built per region (MoP's final update MPQ), so the
        // pinned size must follow the URL or the correct download is rejected
        // as a size mismatch. Regions absent from the map keep the default.
        for (const std::pair<std::string, long long>& rs : m.regionSizes)
        {
            if (rs.first == region)
            {
                m.size = rs.second;
            }
        }
    }
    for (Artifact& a : run.artifacts)
    {
        if (a.outName == "WoW.mfil")
        {
            a.content =
                region_rewrite_mfil(a.content, base.regionManifests, region);
        }
    }
}

void verify_or_throw(const Bytes& data, const std::string& expectedMd5,
                     const std::string& name)
{
    std::string got = md5_hex(data);
    if (got != expectedMd5)
    {
        throw std::runtime_error("MD5 mismatch for " + name + ": expected " +
                                 expectedMd5 + " got " + got);
    }
}

std::vector<std::string> artifact_part_paths(const Recipe& r,
                                             const std::string& outDir)
{
    // Mirrors the `part = dst + ".part"` naming used by reconstruct() below;
    // the .fb sibling is legacy scratch from the removed region failover.
    // Root-level source archives download to a .part sibling now, but older
    // builds fetched them in place under their final name, so BOTH names are
    // swept: a stale raw-name archive must not survive a fresh start either
    // (MoP's final update MPQ genuinely differs per region).
    std::vector<std::string> out;
    out.reserve(r.artifacts.size() * 2 + (r.mpqs.size() + r.zips.size()) * 2);
    for (const Artifact& a : r.artifacts)
    {
        const std::string dst = outDir + "/" + a.outName;
        out.push_back(dst + ".part");
        out.push_back(dst + ".part.fb");
    }
    for (const MpqSource& m : r.mpqs)
    {
        const std::string base =
            outDir + "/" + m.url.substr(m.url.find_last_of('/') + 1);
        out.push_back(base + ".part");
        out.push_back(base);
    }
    for (const ZipSource& z : r.zips)
    {
        const std::string base =
            outDir + "/" + z.url.substr(z.url.find_last_of('/') + 1);
        out.push_back(base + ".part");
        out.push_back(base);
    }
    return out;
}

std::string recipe_id(const Recipe& r)
{
    // Field separators keep adjacent values from colliding ("a"+"bc" vs
    // "ab"+"c"). Sizes participate so a manifest revision that changes only a
    // byte count still changes the digest.
    std::string ident;
    for (const MpqSource& m : r.mpqs)
    {
        ident += "M|" + m.url + "|" + std::to_string(m.size) + "\n";
    }
    for (const ZipSource& z : r.zips)
    {
        ident += "Z|" + z.url + "\n";
    }
    for (const Artifact& a : r.artifacts)
    {
        ident += "A|" + a.outName + "|" + std::to_string(a.size) + "|" +
                 a.url + "|" + a.md5 + "\n";
    }
    return md5_hex(Bytes(ident.begin(), ident.end()));
}

void remove_build_scratch(const std::string& outDir,
                          const std::vector<std::string>& sourceFiles)
{
    // Never the journal here: this also runs from reconstruct()'s failure
    // path, where the journal and the .part files must survive so the next
    // run can prove the partials are its own and resume them. The journal is
    // removed explicitly on the full-success path only.
    std::error_code ec;
    for (const std::string& f : sourceFiles)
    {
        std::filesystem::remove(outDir + "/" + f, ec);
    }
}

namespace
{
/// Return the final path segment (file name) of a URL.
std::string url_basename(const std::string& url)
{
    return url.substr(url.find_last_of('/') + 1);
}
} // namespace

void reconstruct(const Recipe& r, const std::string& outDir)
{
    reconstruct(r, outDir, ReconstructOpts{});
}

void reconstruct(const Recipe& r, const std::string& outDir,
                 const ReconstructOpts& opts)
{
    std::filesystem::create_directories(outDir);

    // These are declared before the try so the catch block and the post-try
    // success reporting can access them.
    std::map<std::string, Bytes> zipCache;
    std::vector<std::string> zipSourceFiles;
    std::vector<std::string> mpqSourceFiles;
    std::map<std::string, std::unique_ptr<MpqArchive>> mpqCache;
    int reportSizeChecked = 0;
    long long reportTotalBytes = 0;

    // A journal may authorise skips and .part resumption ONLY when its stamp
    // provably describes THIS reconstruction. Region/manifest/torrent are the
    // orchestrator's to check (journal_matches), but the recipe digest can and
    // must be verified right here: a stamped journal from different work
    // handed in by a library caller would otherwise authorise correct-size
    // skips, and its partials would be resumed with this recipe's bytes.
    // Untrusted => nothing is skipped and every download starts from byte 0,
    // overwriting any stale .part instead of appending to it.
    const bool journalTrusted = opts.journal && opts.journal->stamped &&
                                opts.journal->stamp.recipeId == recipe_id(r);

    try
    {
    // Download each zip source once and keep its bytes in memory.  A zip
    // lands under a .part name and only ever keeps that name while it is
    // incomplete, so an interrupted run can never leave a partial archive
    // looking like a finished one; a finished zip is kept and reused as-is by
    // later runs of the same recipe instead of being fetched again.
    for (const ZipSource& z : r.zips)
    {
        const std::string base = url_basename(z.url);
        const std::string dest = outDir + "/" + base;
        const std::string part = dest + ".part";
        std::error_code zec;
        bool useLegacy = false;
        if (std::filesystem::exists(part, zec))
        {
            // Finish the earlier download; a .part that is already complete
            // makes the server answer 416, which download_file treats as
            // done, so this is instant in that case.
            printf("Resuming %s from a previous download...\n", base.c_str());
            DownloadOpts zopts;
            zopts.progress_label = base;
            download_file(z.url, part, zopts);
        }
        else if (std::filesystem::exists(dest, zec))
        {
            // No .part but the final name exists: a zip left by an older
            // in-place build.  Its bytes are validated by the zip parsing
            // below; a corrupt one throws, gets swept, and is re-fetched.
            useLegacy = true;
            printf("Using existing %s (from an earlier run)...\n",
                   base.c_str());
        }
        else
        {
            printf("Downloading %s...\n", base.c_str());
            DownloadOpts zopts;
            zopts.progress_label = base;
            download_file(z.url, part, zopts);
        }
        // Track BOTH names BEFORE read_file: if read_file throws (e.g. a disk
        // error after a successful download), the exception path below needs
        // the list populated so it can remove the stranded archive.  They are
        // intentionally NOT removed here any more -- keeping them is what
        // lets the reuse branch above skip the fetch on the next run; the
        // full-success cleanup at the end of reconstruct() removes them.
        zipSourceFiles.push_back(base + ".part");
        if (useLegacy)
        {
            zipSourceFiles.push_back(base);
        }
        zipCache[z.key] = read_file(useLegacy ? dest : part);
    }

    // Download each MPQ source once and open it for reading.  Same .part
    // discipline, plus a size pre-check: the expected byte count is pinned
    // per source, so an archive already sitting at the final name with
    // exactly that size needs no network at all.
    for (const MpqSource& m : r.mpqs)
    {
        const std::string base = url_basename(m.url);
        const std::string dest = outDir + "/" + base;
        const std::string part = dest + ".part";
        std::error_code mec;
        bool ready = false;
        if (m.size >= 0 && !std::filesystem::exists(part, mec))
        {
            std::error_code sec;
            const auto have = std::filesystem::file_size(dest, sec);
            if (!sec && static_cast<long long>(have) == m.size)
            {
                printf("  %-28s already complete (checked)\n", base.c_str());
                ready = true;
            }
            else if (!sec)
            {
                // A file of the wrong length under the final name is stale
                // (an in-place build's partial, or another region's archive:
                // sizes are pinned per region).  Appending can never fix it.
                std::filesystem::remove(dest, sec);
            }
        }
        if (!ready)
        {
            if (std::filesystem::exists(part, mec))
            {
                printf("Resuming %s from a previous download...\n",
                       base.c_str());
            }
            else
            {
                printf("Downloading %s...\n", base.c_str());
            }
            DownloadOpts mopts;
            mopts.expected_size = m.size;
            mopts.progress_label = base;
            try
            {
                download_file(m.url, part, mopts);
            }
            catch (const SizeMismatchError&)
            {
                // The .part has the wrong length in place; appending more
                // can never fix it.  Remove it so the next run re-downloads
                // from zero instead of failing the same way forever.
                std::error_code rec;
                std::filesystem::remove(part, rec);
                throw;
            }
            // Promote the verified archive to its final name.  An older
            // in-place file of the wrong length was removed above; remove
            // defensively anyway because Windows rename refuses to replace.
            std::filesystem::remove(dest, mec);
            std::filesystem::rename(part, dest);
        }
        // Track BOTH names BEFORE opening: if MpqArchive construction throws
        // (e.g. corrupt MPQ that downloaded at the right size but is
        // invalid), the files are still on disk and must be cleaned up by
        // the exception path below; the missing one is a harmless no-op.
        mpqSourceFiles.push_back(base + ".part");
        mpqSourceFiles.push_back(base);
        mpqCache[m.key] = std::make_unique<MpqArchive>(dest);
    }

    // Produce, verify, and write every artifact.
    for (const Artifact& a : r.artifacts)
    {
        std::string dst = outDir + "/" + a.outName;
        std::filesystem::create_directories(
            std::filesystem::path(dst).parent_path());
        std::string part;
        Bytes data;
        bool skipped = false;
        bool resumeSkip = false;
        bool markPending = false;
        switch (a.source)
        {
            case Source::RepairMd5:
            {
                // These binaries used to be re-fetched on every run, which
                // made a resumed run LOOK like a fresh start.  A file already
                // on disk that hashes to the pinned MD5 is byte-identical to
                // what the repair store would serve: verify it, show it in
                // the same 100% language as every other kept file, and leave
                // the network idle.  A mismatch (truncated/tampered output)
                // falls through to the normal fresh download below.
                std::error_code bec;
                if (std::filesystem::exists(dst, bec))
                {
                    Bytes existing = read_file(dst);
                    if (md5_hex(existing) == a.md5)
                    {
                        // Route through the shared skipped path: it prints
                        // the same 100% line as every other kept file and
                        // continues before the common MD5 verification (data
                        // is intentionally left empty here).
                        skipped    = true;
                        resumeSkip = true;
                        break;
                    }
                }
                part = dst + ".part";
                DownloadOpts dopts;
                dopts.resume = false;
                dopts.progress_label = a.outName;
                download_file(repair_url(r.repairBase, a.md5), part, dopts);
                data = read_file(part);
                break;
            }
            case Source::ZipMember:
            {
                auto it = zipCache.find(a.zipKey);
                if (it == zipCache.end())
                {
                    throw std::runtime_error("unknown zipKey: " + a.zipKey);
                }
                data = zip_extract_member(it->second, a.outName);
                break;
            }
            case Source::MpqPtch:
            {
                auto base = mpqCache.find(a.baseMpqKey);
                auto patch = mpqCache.find(a.patchMpqKey);
                if (base == mpqCache.end() || patch == mpqCache.end())
                {
                    throw std::runtime_error("unknown mpqKey for " + a.outName);
                }
                Bytes b = base->second->extract(a.basePath);
                Bytes p = patch->second->extract(a.patchPath);
                data = apply_ptch(b, p);
                break;
            }
            case Source::MpqExtract:
            {
                auto src = mpqCache.find(a.baseMpqKey);
                if (src == mpqCache.end())
                {
                    throw std::runtime_error("unknown mpqKey for " + a.outName);
                }
                data = src->second->extract(a.basePath);
                if (!a.patchMpqKey.empty())
                {
                    auto patch = mpqCache.find(a.patchMpqKey);
                    if (patch == mpqCache.end())
                    {
                        throw std::runtime_error("unknown patchMpqKey for " +
                                                 a.outName);
                    }
                    data = apply_ptch(data, patch->second->extract(a.patchPath));
                }
                break;
            }
            case Source::PlainUrl:
            {
                part = dst + ".part";
                std::filesystem::create_directories(
                    std::filesystem::path(dst).parent_path());
                DownloadOpts popts;
                // Resume only partials this run can prove are its own (see
                // journalTrusted above); resume=false opens "wb" and
                // overwrites, so a foreign .part never receives appended
                // bytes.
                popts.resume = journalTrusted;
                popts.expected_size = a.size;
                // Say "resuming" on the progress line when a partial from an
                // earlier run is about to be continued, so a live transfer
                // and a continued one are distinguishable at a glance.
                std::string label = a.outName;
                std::error_code pec;
                if (popts.resume && std::filesystem::exists(part, pec))
                {
                    label += " (resuming)";
                }
                popts.progress_label = label;
                if (journalTrusted && is_done(*opts.journal, a.outName))
                {
                    // Only skip if the destination file is actually on disk.
                    // When a size is known, also confirm the file size matches
                    // so a truncated output is re-downloaded, not silently
                    // accepted as complete.
                    std::error_code fec;
                    bool dstOk = std::filesystem::exists(dst, fec);
                    if (dstOk && a.size >= 0)
                    {
                        auto fsz = std::filesystem::file_size(dst, fec);
                        if (fec || static_cast<long long>(fsz) != a.size)
                        {
                            dstOk = false;
                        }
                    }
                    if (dstOk)
                    {
                        skipped    = true;
                        resumeSkip = true;
                        break;
                    }
                    // File missing or wrong size — fall through and re-download.
                }
                // NOTE: there is deliberately no cross-region failover here.
                // Substituting the other region's copy cannot be validated at
                // this layer: Data artifacts carry no MD5, the fallback's
                // authentic size lives only in that region's manifest, and
                // several archives genuinely differ per region (MoP's
                // Updates/wow-0-18414-Win-final.MPQ). A fallback that happened
                // to match the selected region's expected length would be
                // accepted without anything establishing byte-identity, so a
                // pod outage now surfaces as a clean failure the user can
                // retry rather than as a possibly-wrong client.
                try
                {
                    download_file(a.url, part, popts);
                }
                catch (const SizeMismatchError&)
                {
                    // The .part has the wrong length in place (e.g. a server
                    // once answered 200 to a Range request and its body was
                    // appended).  Appending can never shrink it back, so
                    // discard it: the next run then starts this file from
                    // zero instead of failing the same way forever.
                    std::error_code rec;
                    std::filesystem::remove(part, rec);
                    throw;
                }
                catch (const std::exception&)
                {
                    if (a.optional)
                    {
                        std::error_code rec;
                        std::filesystem::remove(part, rec);
                        skipped = true;
                        break;
                    }
                    // Non-optional network failures keep the .part on disk:
                    // the bytes fetched so far are exactly what a resumed run
                    // continues from.
                    throw;
                }
                // Only load bytes into RAM when an MD5 must be verified.
                // Data files carry no MD5, so multi-GB files stay on disk and
                // are size-checked via file_size + (optionally) streamed for
                // piece verification.
                if (!a.md5.empty())
                {
                    data = read_file(part);
                }
                if (opts.torrent != nullptr)
                {
                    printf("  Verifying %s (piece SHA-1)...\n",
                           a.outName.c_str());
                    try
                    {
                        verify_file_pieces(*opts.torrent, a.outName, part);
                    }
                    catch (...)
                    {
                        std::error_code rec;
                        std::filesystem::remove(part, rec);
                        throw;
                    }
                }
                // Trusted journals only, for WRITES as much as reads: a
                // foreign journal must never gain completion lines from this
                // recipe, or a later run matching the FOREIGN stamp would
                // skip a same-sized file this recipe produced.
                if (journalTrusted && !skipped)
                {
                    markPending = true;
                }
                break;
            }
            case Source::Generated:
            {
                data = Bytes(a.content.begin(), a.content.end());
                break;
            }
        }
        if (skipped)
        {
            if (resumeSkip)
            {
                // Resumed files speak the same visual language as a live
                // download, pinned at 100%, so the work kept from the last
                // run and the work left read as one continuous list.
                std::error_code fszec;
                long long done = a.size;
                if (done < 0)
                {
                    done = static_cast<long long>(
                        std::filesystem::file_size(dst, fszec));
                }
                if (fszec)
                {
                    done = 0;
                }
                printf("%s\n",
                       wcr::render_progress(a.outName, done, done, -1)
                           .c_str());
            }
            else
            {
                printf("  %-28s SKIPPED (optional, source missing)\n",
                       a.outName.c_str());
            }
            continue;
        }
        // Size comes from disk for download-to-disk sources (RepairMd5,
        // PlainUrl) so multi-GB Data files are never loaded into RAM; in-memory
        // sources (ZipMember/MpqPtch/Generated) use the buffer length.
        long long actualSize = part.empty()
            ? static_cast<long long>(data.size())
            : static_cast<long long>(std::filesystem::file_size(part));
        // Verification: for PlainUrl, discard the .part file on mismatch so
        // a re-run re-downloads from scratch (corrupt bytes, not just network
        // failure which must remain resumable).
        try
        {
            if (!a.md5.empty())
            {
                verify_or_throw(data, a.md5, a.outName);
            }
            if (a.size >= 0 && actualSize != a.size)
            {
                throw std::runtime_error(
                    "size mismatch for " + a.outName + ": expected " +
                    std::to_string(a.size) + " got " +
                    std::to_string(actualSize));
            }
        }
        catch (...)
        {
            // Discard any .part file on verification failure so that a re-run
            // downloads from scratch rather than resuming corrupt bytes.
            // Covers both PlainUrl and RepairMd5 (both download to `part`).
            if (!part.empty())
            {
                std::error_code rec;
                std::filesystem::remove(part, rec);
            }
            throw;
        }
        if (a.size >= 0)
        {
            ++reportSizeChecked;
        }
        if (a.source == Source::RepairMd5 || a.source == Source::PlainUrl)
        {
            std::filesystem::rename(part, dst);
        }
        else
        {
            write_file(dst, data);
        }
        printf("  %-28s %lld bytes  [%s]\n", a.outName.c_str(), actualSize,
               a.md5.c_str());
        reportTotalBytes += actualSize;
        if (markPending && journalTrusted)
        {
            mark_done(*opts.journal, a.outName);
        }
    }
    } // end try
    catch (...)
    {
        // On any failure: close open MPQ handles first (StormLib keeps file
        // handles open; Windows blocks deletion of open files), then remove
        // whatever scratch files were downloaded before the failure.
        // zipSourceFiles covers the case where read_file throws after the zip
        // was successfully downloaded but before the inline remove(); the
        // cleanup is a harmless no-op if the file is already gone.
        mpqCache.clear();
        remove_build_scratch(outDir, zipSourceFiles);
        remove_build_scratch(outDir, mpqSourceFiles);
        throw;
    }

    // Success: strip the build scratch so the output is a pristine client
    // (the source MPQs are already correctly present in Data/ and Updates/).
    // This is only reached after the artifact loop completes, i.e. on FULL
    // success — any failed/interrupted artifact throws earlier and never gets
    // here, so the journal has nothing left to resume and is safe to remove.
    // (The journal is removed HERE and only here; the failure path keeps it so
    // the next run can resume its partials.)
    // Close the MPQ handles FIRST: while an MpqArchive is open, StormLib keeps
    // the source .MPQ open and Windows refuses to delete an open file (the
    // remove fails silently via error_code), stranding the scratch in the root.
    mpqCache.clear();
    // Zips are no longer removed inline after their bytes are read (a kept
    // archive is what lets a later run reuse it), so the success cleanup must
    // strip them here alongside the MPQ sources.
    remove_build_scratch(outDir, zipSourceFiles);
    remove_build_scratch(outDir, mpqSourceFiles);
    {
        std::error_code jec;
        std::filesystem::remove(outDir + "/.wcr-journal", jec);
    }

    int reportPieceVerified = (opts.torrent != nullptr) ? reportSizeChecked : 0;
    printf("\n=== Reconstruction complete ===\n");
    printf("  artifacts written : %zu\n", r.artifacts.size());
    printf("  total bytes       : %lld\n", reportTotalBytes);
    printf("  size-checked      : %d\n", reportSizeChecked);
    printf("  piece-verified    : %d\n", reportPieceVerified);
    printf("\nFinal safety state:\n");
    printf("  CDN reconstruction complete\n");
    printf("  Blizzard files distributed by this tool: no\n");
}
} // namespace wcr
