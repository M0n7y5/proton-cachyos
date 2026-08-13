/*
 * Test double for the driver's snapshot publisher.  It creates the same file at
 * the same path and writes through the same seqlocks and the same
 * pwhud_flags_publish() helper the driver uses, so a consumer bug cannot hide
 * behind a hand-rolled layout.  Test scaffolding, never shipped.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WINEPIPEWIRE_HUD_TESTS_PUBLISHER_HPP
#define WINEPIPEWIRE_HUD_TESTS_PUBLISHER_HPP

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/snapshot.hpp"

struct hud_publisher
{
    struct pwhud_snapshot *snap = nullptr;
    char path[4096] = {};

    /* Uses the reader's own path builder, so the two cannot disagree about
     * where the file lives.  The corollary bites: with HUD_ENV_PID set, the
     * layer is pointed at another process's snapshot and this resolves to that
     * same file, so publishing here overwrites what was to be observed.  See the
     * warning in present_smoke.cpp's header. */
    bool open(void)
    {
        char dir[4096];
        char *slash;
        int fd;

        if (!hud_snapshot_path(path, sizeof(path)))
        {
            fprintf(stderr, "publisher: cannot build a snapshot path, is HOME set\n");
            return false;
        }
        snprintf(dir, sizeof(dir), "%s", path);
        if (!(slash = strrchr(dir, '/')))
            return false;
        *slash = 0;
        if (mkdir(dir, 0700) && errno != EEXIST)
        {
            fprintf(stderr, "publisher: mkdir %s: %s\n", dir, strerror(errno));
            return false;
        }
        if ((fd = ::open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600)) < 0)
        {
            fprintf(stderr, "publisher: open %s: %s\n", path, strerror(errno));
            return false;
        }
        if (ftruncate(fd, PWHUD_BYTES))
        {
            fprintf(stderr, "publisher: ftruncate %s: %s\n", path, strerror(errno));
            close(fd);
            return false;
        }
        snap = (struct pwhud_snapshot *)mmap(nullptr, PWHUD_BYTES, PROT_READ | PROT_WRITE,
                                             MAP_SHARED, fd, 0);
        close(fd);
        if (snap == MAP_FAILED)
        {
            snap = nullptr;
            fprintf(stderr, "publisher: mmap %s: %s\n", path, strerror(errno));
            return false;
        }
        memset(snap, 0, sizeof(*snap));
        snap->version = PWHUD_VERSION;
        snap->size = sizeof(*snap);
        snap->writer_pid = (uint32_t)getpid();
        /* Magic last, so a reader never sees a valid header over an
         * uninitialised page. */
        __atomic_store_n(&snap->magic, PWHUD_MAGIC, __ATOMIC_RELEASE);
        return true;
    }

    void close_and_unlink(void)
    {
        if (snap)
            munmap(snap, PWHUD_BYTES);
        snap = nullptr;
        if (*path)
            unlink(path);
    }

    /* Section A, seqlock A.  Split into begin and end so a test can leave the
     * sequence odd and stand in for a writer caught mid-update. */
    void a_begin(void)
    {
        uint32_t seq = __atomic_load_n(&snap->seq_drv, __ATOMIC_RELAXED);

        __atomic_store_n(&snap->seq_drv, seq + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }

    void a_end(void)
    {
        uint32_t seq = __atomic_load_n(&snap->seq_drv, __ATOMIC_RELAXED);

        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&snap->seq_drv, seq + 1, __ATOMIC_RELAXED);
    }

    void b_begin(void)
    {
        uint32_t seq = __atomic_load_n(&snap->seq_sp, __ATOMIC_RELAXED);

        __atomic_store_n(&snap->seq_sp, seq + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);
    }

    void b_end(void)
    {
        uint32_t seq = __atomic_load_n(&snap->seq_sp, __ATOMIC_RELAXED);

        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&snap->seq_sp, seq + 1, __ATOMIC_RELAXED);
    }
};

#endif /* WINEPIPEWIRE_HUD_TESTS_PUBLISHER_HPP */
