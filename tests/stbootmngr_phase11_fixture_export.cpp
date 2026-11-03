#define STBOOT_PHASE11_EXPORT_ONLY
#include "tests/stbootmngr_basesystem_selftest.cpp"
#undef STBOOT_PHASE11_EXPORT_ONLY

// Declare only the small Linux host API needed by this fixture exporter.
// Do not include libc headers here: the included Starlix headers define
// their own global fixed-width integer and size aliases.
extern "C" {
int open(const char* pathname, int flags, ...);
long read(int fd, void* buffer, unsigned long count);
long write(int fd, const void* buffer, unsigned long count);
long lseek(int fd, long offset, int whence);
int close(int fd);
}

static bool WriteAll(int fd, const void* data, unsigned long long length) {
    const char* bytes = static_cast<const char*>(data);
    unsigned long long done = 0;

    while (done < length) {
        const long n = write(
            fd,
            bytes + done,
            static_cast<unsigned long>(length - done)
        );
        if (n <= 0) return false;
        done += static_cast<unsigned long long>(n);
    }
    return true;
}

static int Error(const char* message, int code) {
    unsigned long long n = 0;
    while (message[n] != '\0') ++n;
    (void)WriteAll(2, message, n);
    (void)WriteAll(2, "\n", 1);
    return code;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        return Error(
            "Usage: phase11_fixture_export <test-child.efi> <output-rxfs.img>",
            2
        );
    }

    BuildFileImage();

    const int child = open(argv[1], 0); // O_RDONLY
    if (child < 0) return Error("ABORT: cannot open EFI test child", 3);

    const long childSize = lseek(child, 0, 2); // SEEK_END
    if (childSize <= 0 || childSize > static_cast<long>(BS)) {
        (void)close(child);
        return Error("ABORT: EFI child must fit in one 4096-byte APFS extent", 4);
    }
    if (lseek(child, 0, 0) < 0) { // SEEK_SET
        (void)close(child);
        return Error("ABORT: cannot rewind EFI test child", 5);
    }

    // The APFS fixture gives boot.efi two 4096-byte extents and a 4100-byte
    // logical size. Clear both extents, then place the EFI child at offset 0.
    E2eZero(Image + 14 * BS, 2 * BS);

    unsigned long long got = 0;
    while (got < static_cast<unsigned long long>(childSize)) {
        const long n = read(
            child,
            Image + 14 * BS + got,
            static_cast<unsigned long>(
                static_cast<unsigned long long>(childSize) - got
            )
        );
        if (n <= 0) {
            (void)close(child);
            return Error("ABORT: failed to read complete EFI test child", 6);
        }
        got += static_cast<unsigned long long>(n);
    }
    if (close(child) != 0) {
        return Error("ABORT: failed to close EFI test child", 7);
    }

    E2eBuildDmg(false);
    if (E2eDmgSize <= sizeof(Image) || E2eDmgSize > sizeof(E2eDmgImage)) {
        return Error("ABORT: synthetic DMG size is invalid", 8);
    }

    E2eBuildRxfs();

    // Linux O_WRONLY | O_CREAT | O_TRUNC, mode 0644.
    const int out = open(argv[2], 1 | 0100 | 01000, 0644);
    if (out < 0) return Error("ABORT: cannot create RXFS fixture", 9);

    const bool writeOk = WriteAll(out, E2eRxfsImage, sizeof(E2eRxfsImage));
    const int closeStatus = close(out);
    if (!writeOk || closeStatus != 0) {
        return Error("ABORT: failed to write complete RXFS fixture", 10);
    }

    (void)WriteAll(1, "PASS: exported synthetic RXFS fixture with EFI child\n", 53);
    return 0;
}
