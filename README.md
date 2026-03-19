# RendezvOS_Linux

This repository provides a Linux-compatible layer on top of RendezvOS.

## Build layout

The project now has two build scopes:

- `core/` remains the self-contained kernel build tree, its the RendezvOS core. It can still be built and run independently for kernel-side experiments and tests.
- The repository root is the integration layer. It owns the Linux-compatibility sources, the user-test workflow, and the final top-level build.

## Build targets

### `make ... build`

`build` is the top-level orchestration target.

It does the following:

1. Ensures the architecture is configured.
2. Ensures the top-level user payload exists.
3. Compiles the root compatibility-layer objects.
4. Invokes the `core/` build to produce the final kernel image.

Use this when you want the full, final image that combines `core/` with the Linux compatibility layer.

### `make ... build_lib`

`build_lib` is the lower-level build stage used by the top-level build.

It does the actual artifact construction work:

1. Verifies the user payload has already been generated.
2. Builds the root-level compatibility objects into `build/root/`.
3. Calls into `core/` with the extra root objects so the final kernel image can be linked.

Use this when you want to verify the integration build step directly, or when you are wiring the project into another wrapper.

## User test flow

The user-test payload is now managed from the repository root.

Typical sequence:

```bash
make ARCH=x86_64 config
make ARCH=x86_64 user
make ARCH=x86_64 build
make ARCH=x86_64 run
```

`make user` clones or updates the user test repository under `build/user_payload/`, and produces the payload object that will be linked into the `.user` section.

When running through the top-level wrapper (`make ... run` / `make ... dump`):

- `LOG=true` writes `qemu.log` to the repository root.
- `DUMP=true` writes `objdump.log` to the repository root.

## Standalone `core/` mode

You can still build `core/` by itself if you want kernel-only tests.

```bash
cd core
make ARCH=x86_64 config
make user
make run
```

In this mode, `core/` keeps its own `build/` directory and produces the standalone kernel image exactly as before.

## Common commands

```bash
# Configure the top-level integration build
make ARCH=x86_64 config

# Generate the user payload
make ARCH=x86_64 user

# Build the final integrated image
make ARCH=x86_64 build

# Run the integrated image
make ARCH=x86_64 run

# Build only the integration stage
make ARCH=x86_64 build_lib

# Clean generated files
make ARCH=x86_64 clean

# Remove all generated files and user payloads
make ARCH=x86_64 mrproper
```

## Notes

- The repository root keeps `build/` for the integrated build output.
- `core/` keeps `core/build/` for standalone kernel builds.
- Both directories are ignored by Git.
- The root `Makefile` is intentionally tracked and should not be ignored.