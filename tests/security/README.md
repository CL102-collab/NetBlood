# Archive and resource parser regressions

These asset-free tests exercise the production archive loader and Blood resource
registration code with malformed input and valid boundary cases. They require a
C++ compiler, Python 3, `pkg-config`, and SDL2 development headers.

Run all checks, including AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
for test in tests/security/*-test.sh; do
    SANITIZE=1 "$test"
done
```

Omit `SANITIZE=1` for an ordinary build. In the prepared cloud environment, source
`/workspace/netblood-env/activate.sh` first. Fixtures and executables are created
in temporary directories; no commercial game assets are required.

The CI unit gate discovers these scripts automatically and runs them with
sanitizers before any packages can be built. These checks cover archive count,
directory, entry-size and offset validation, truncated archives, and oversized
resource names, types and paths. They also check that valid resources remain
readable and rejected input does not corrupt existing state.

This is targeted coverage of these parser bugs, not a sandbox or a security
audit of every addon format. Recursive DEF handling is unchanged.
