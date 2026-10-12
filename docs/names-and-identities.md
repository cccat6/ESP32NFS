# UTF-8 names and file identities

ESP32NFS accepts valid UTF-8 file and directory names. Chinese names, mixed scripts, accented letters and supplementary characters share the same file operations and directory enumeration APIs.

## SdFat configuration

Build the application, ESP32NFS and SdFat 2.3.0 with these definitions:

```text
-DUSE_LONG_FILE_NAMES=1 -DUSE_UTF8_LONG_NAMES=1
```

The ESP32 SD examples include `build_opt.h` with these options. For Arduino CLI, including Pico builds, pass them through the C++ build properties:

```sh
arduino-cli compile --fqbn rp2040:rp2040:rpipicow \
  --build-property 'compiler.cpp.extra_flags=-DUSE_LONG_FILE_NAMES=1 -DUSE_UTF8_LONG_NAMES=1' \
  --library /path/to/ESP32NFS /path/to/ESP32NFS/examples/PicoSdCardShare
```

Append these definitions to an application's existing build flags. Apply them to every translation unit, including SdFat's source files. `SdFatStorage` checks the configuration at compilation.

## Stored spelling and comparison

FAT long names and exFAT names are read through SdFat's UTF-8 interface. Name lookup uses the backend's own comparison rules. `SdFatStorage` resolves each directory component and obtains its stored name by directory-entry index, including when a request uses a FAT 8.3 alias.

The backend uses SdFat 2.3.0's built-in Unicode upcase mapping for comparisons. This is the mapping used by that dependency's FAT/exFAT lookup and name hashing. It also defines the library's explicit case-only rename behavior. The [exFAT specification](https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification) describes the volume upcase table; SdFat's matching implementation uses its compiled mapping.

Names retain their stored encoding form. NFC `é.txt` and NFD `é.txt` are independently addressable names. Explicit renames set the requested new spelling. Replacing an existing destination retains that destination's stored name, including when the request addresses it through an alias. Renaming an object to its own short alias completes as a same-object operation.

Validation checks UTF-8 scalar encoding, path boundaries and the filesystem's naming rules. FAT validation checks reserved characters, control bytes, leading spaces and trailing ASCII dots/spaces. Full canonical paths follow the library's byte budget, including paths expanded from short aliases.

## Canonical path API

`PathVfs` provides read-only resolution for application policies:

```cpp
uint64_t object;
Status status = files.resolvePath("/资料/FILE~1.BMP", object);
if (status == Ok) {
  char canonical[PathSize];
  status = files.canonicalPath(object, canonical, sizeof(canonical));
  // canonical contains the stored absolute path within this filesystem root.
}
```

`resolvePath()` accepts an absolute path with explicit components. `canonicalPath()` returns the registered stored spelling for a live file identity. Long names, short aliases and native case aliases converge on one ID and one path. IDs remain associated with the object during library-managed file or directory renames. Deletion retires the ID, and a new object receives a new ID.

Scope IDs to the owning `PathVfs` instance and filesystem session. Multiple export aliases can share that same `PathVfs`. Canonical-path policies include the underlying volume/root identity in their key. External namespace mutations and remounts use `invalidateAll()` before service resumes; this refreshes identities and cached paths for the new namespace state.

## File protection and leases

Perform resolution, authorization and I/O under the same volume lease:

1. Resolve each existing source or destination and obtain its canonical path or scoped ID.
2. Check active file leases against the resolved identity.
3. For creation, resolve the parent and check an existing destination before applying creation attributes or truncation.
4. For rename or directory removal, check the source, destination and applicable descendant leases.
5. Execute the operation within that lease scope.

This lets application policies recognize every spelling of the same file. A policy migration from spelling-based checks adopts resolved checks across all mutating operations, including unchecked CREATE and SETATTR. Authorization uses the resolved byte spelling without further normalization.

## Backend integration

`PathStorage::resolve(path, output, capacity, attributes)` supplies a stored absolute name and attributes. Output can share the input buffer. Alias-providing backends implement this operation; the default implementation serves case-sensitive namespaces. `validateName()` supplies filesystem-specific component validation. `sameName()` supplies native case equivalence for explicit case renames.

Directory `entry()` and `entryInfo()` return stored names. Implementations coordinate external mutations through the same volume ownership and cache lifecycle as normal file access.
