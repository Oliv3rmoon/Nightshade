// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace Eden {
// Returns zero when the app can write to its existing data mount, or the
// filesystem error. Only the app data directory and its own probe are touched.
inline int MountedStorageStatus(const char* directory) {
    struct stat status{};
    if (stat(directory, &status) == 0) {
        if (!S_ISDIR(status.st_mode)) return ENOTDIR;
    } else {
        if (errno != ENOENT) return errno;
        if (mkdir(directory, 0755) != 0 && errno != EEXIST) return errno;
    }

    const std::string prefix = std::string{directory} + "/.nightshade-access-" +
                               std::to_string(getpid()) + "-";
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const std::string path = prefix + std::to_string(attempt);
        const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return errno;
        }

        // The open descriptor stays usable after its name is removed.
        int error = unlink(path.c_str()) == 0 ? 0 : errno;
        if (!error) {
            const char value = 'N';
            ssize_t written;
            do {
                written = write(fd, &value, sizeof(value));
            } while (written < 0 && errno == EINTR);
            if (written != 1) error = written < 0 ? errno : EIO;
        }
        if (close(fd) != 0 && !error) error = errno;
        return error;
    }
    return EEXIST;
}
} // namespace Eden
