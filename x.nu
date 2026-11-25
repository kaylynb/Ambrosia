# The Ambrosia build tool, the same on Windows and Linux: `nu x.nu <command>`, or `mise run x <command>`.
# mise pins nu and the other tools (mise.toml); this file drives CMake and the containers.
#
#     nu x.nu build [preset]              configure and build a preset (default release)
#     nu x.nu build asan --test           and run its tests
#     nu x.nu build release --container   in the Debian 13 container (container/Containerfile)
#     nu x.nu build switch --tracy        the Switch homebrew build, always in its container
#     nu x.nu run ambrosia 02_NYC_BatteryPark
#     nu x.nu format [--check]
#     nu x.nu runner status               the Switch runner (scripts/switch-runner.py)
#
# `nu x.nu build --help` and so on list each command's options.

const ROOT = path self .

const PRESETS = [release debug asan tsan mingw switch]
const TESTED = [release debug asan tsan]
# Memory a test of a sanitizer preset takes at most, in GB, which sets how many run at once.
# tsan's is gameplay_test's peak (5.1 GB); world_test takes 4.4 GB, most others 3.
const TEST_GB = {asan: 2, tsan: 5}
# Docker first on Windows, where Docker Desktop is the usual engine and wslc can stop answering;
# elsewhere wslc is never installed and Podman comes before Docker.
const WINDOWS_RUNTIMES = [docker wslc podman]
const RUNTIMES = [wslc podman docker]
const DESKTOP_IMAGE = "ambrosia-build"
const SWITCH_IMAGE = "ambrosia-switch"
const MESA_IMAGE = "ambrosia-switch-mesa"
const CCACHE_VOLUME = "ambrosia-ccache"

def main [] {
    print "usage: nu x.nu <build|package|verify-package|run|format|runner> ...; nu x.nu <command> --help for its options"
}

# Build the desktop distribution on its target OS. This build tree is independent of development.
def "main package" [
    --version: string       # version without the v tag prefix
    --commit: string        # full Git revision (defaults to HEAD)
    --container             # Linux only: build and test in the Debian 13 container, in build/container-package
    --runtime: string       # docker or podman with --container (default: $CONTAINER_RUNTIME, else the first that answers)
] {
    cd $ROOT
    if ($version | default "") !~ '^[0-9]+\.[0-9]+\.[0-9]+(-[A-Za-z0-9]+([.-][A-Za-z0-9]+)*)?$' {
        fail "supply --version X.Y.Z or X.Y.Z-suffix"
    }
    let revision = $commit | default { ^git rev-parse HEAD | str trim }
    if $revision !~ '^[0-9a-f]{40}$' { fail "expected a full Git commit" }
    exec-checked ["actionlint"]
    let win = $nu.os-info.name == "windows"
    if $nu.os-info.name not-in [linux windows] or $nu.os-info.arch != "x86_64" { fail "desktop packages require a native x64 Windows or Linux host" }
    # The smoke test runs the packaged programs on this machine, so a container package needs a
    # Linux host, and one whose glibc is at least the container's.
    if $container and $win { fail "--container packages the Linux build, which needs a Linux host to verify it" }
    let dir = $ROOT | path join build (if $container { "container-package" } else { "package" })
    let out = $dir | path join artifacts
    let bin = if $win { $dir | path join bin Release } else { $dir | path join bin }
    let name = $"ambrosia-($version)-($nu.os-info.name)-x64"
    let archive = $out | path join $"($name).(if $win { 'zip' } else { 'tar.gz' })"
    let steps = [
        (["cmake" "--preset" "release" "-B" $dir "-DAMBROSIA_DEV_BUILD=OFF" "-DAMBROSIA_DATA_TESTS=OFF" "-DAMBROSIA_STEAM_AUDIO=ON" $"-DAMBROSIA_VERSION=($version)" $"-DAMBROSIA_COMMIT=($revision)"] ++ (if $win { ["-DAMBROSIA_RELEASE_SYMBOLS=ON"] } else { [] }))
        ["cmake" "--build" $dir "--config" "Release"]
        ["ctest" "--test-dir" $dir "-C" "Release" "--output-on-failure" "-j2"]
    ]
    if $container {
        let rt = find-runtime $runtime --skip ["wslc"]
        build-image $rt $DESKTOP_IMAGE "Containerfile"
        run-in-container $rt $DESKTOP_IMAGE $steps null (runtime-memory $rt) true
    } else {
        with-env (host-env) {
            for step in $steps { exec-checked $step }
        }
    }
    mkdir $out
    cd $bin
    let programs = [ambrosia dented ucc]
    let files = $programs | each {|p| if $win { $"($p).exe" } else { $p } }
    let runtime = if $win { "phonon.dll" } else { "libphonon.so" }
    exec-checked (["cmake" "-E" "tar" (if $win { "cf" } else { "czf" }) $archive] ++ (if $win { ["--format=zip"] } else { ["--format=gnutar"] }) ++ ["--" ...$files $runtime "LICENSES.txt"])
    checksum $archive
    if $win {
        let symbols = $out | path join $"($name)-symbols.zip"
        exec-checked (["cmake" "-E" "tar" "cf" $symbols "--format=zip" "--"] ++ ($programs | each {|p| $"($p).pdb" }))
        checksum $symbols
    }
    main verify-package $archive --version $version --commit $revision
}

def checksum [file: path] {
    let hash = open --raw $file | hash sha256
    $"($hash)  ($file | path basename)\n" | save -f $"($file).sha256"
}

# Smoke-test the files a player actually downloads, outside the checkout and without game data.
def "main verify-package" [archive: path, --version: string, --commit: string] {
    let file = $archive | path expand
    let expected = open --raw $"($file).sha256" | str trim
    let actual = open --raw $file | hash sha256
    if $expected != $"($actual)  ($file | path basename)" { fail "archive checksum mismatch" }
    let win = $nu.os-info.name == "windows"
    let programs = [ambrosia dented ucc] | each {|p| if $win { $"($p).exe" } else { $p } }
    let runtime = if $win { "phonon.dll" } else { "libphonon.so" }
    let members = ^cmake -E tar tf $file | complete
    if $members.exit_code != 0 or ($members.stdout | lines | sort) != ([$runtime "LICENSES.txt" ...$programs] | sort) {
        fail "unexpected package contents"
    }
    let starting_dir = $env.PWD
    let tmp = mktemp -d
    try {
        cd $tmp
        ^cmake -E tar xf $file
        if $env.LAST_EXIT_CODE != 0 { error make {msg: "failed to unpack archive"} }
        for item in ["LICENSES.txt" $runtime] {
            if (ls $item | first | get size) == 0b { error make {msg: $"empty ($item)"} }
        }
        for p in $programs {
            let exe = $tmp | path join $p
            let result = run-external $exe "--version" | complete
            if $result.exit_code != 0 or ($result.stdout | str trim) != $"Ambrosia ($version) \(($commit)\)" {
                error make {msg: $"($p) did not report the expected build: ($result)"}
            }
        }
    } catch {|err|
        cd $starting_dir
        rm -rf $tmp
        fail $err.msg
    }
    cd $starting_dir
    rm -rf $tmp
    print $"Verified ($file)"
}

# Configure and build a preset, in build/[container-]<preset>[-feature...], and with --test run its
# tests. Arguments after the preset that start with -D go to CMake's configure step.
def --wrapped "main build" [
    preset: string = "release"  # release, debug, asan, tsan, mingw or switch
    --test                      # run the tests after building (release, debug, asan, tsan)
    --jobs: int                 # tests to run at once (default: by CPU threads, and memory for asan and tsan)
    --clean                     # remove the build folder first
    --lto                       # link-time optimisation (default only for switch): folder -lto
    --no-lto                    # no link-time optimisation: folder -no-lto
    --tracy                     # the Tracy profiler's client and zones: folder -tracy
    --mesa                      # switch only: Mesa 26.2 from mesa-switch: folder -mesa
    --container                 # build in the container (default with AMBROSIA_CONTAINER=1; always for switch)
    --no-container              # build on this machine even with AMBROSIA_CONTAINER=1
    --runtime: string           # docker, wslc or podman (default: $CONTAINER_RUNTIME, else the first that answers)
    ...cmake_args: string       # -DNAME=VALUE for the configure step
] {
    if (wants-help ([$preset] ++ $cmake_args)) { return (help main build) }
    cd $ROOT
    # A -D given before any preset arrives as the preset.
    let given = if ($preset | str starts-with "-") { {preset: "release", args: ([$preset] ++ $cmake_args)} } else { {preset: $preset, args: $cmake_args} }
    let preset = $given.preset
    if $preset not-in $PRESETS {
        fail $"unknown preset ($preset); the presets are ($PRESETS | str join ', ')"
    }
    if $test and $preset not-in $TESTED {
        fail $"the ($preset) preset has no tests to run here"
    }
    if $test { exec-checked ["actionlint"] }
    if $mesa and $preset != "switch" {
        fail "--mesa is for the switch preset"
    }
    if $lto and $no_lto {
        fail "--lto and --no-lto together"
    }
    let containered = use-container $preset $container $no_container
    let dir = build-dir $preset $containered {lto: $lto, no_lto: $no_lto, tracy: $tracy, mesa: $mesa}

    mut defines = []
    if $lto { $defines ++= ["-DAMBROSIA_LTO=ON"] }
    if $no_lto { $defines ++= ["-DAMBROSIA_LTO=OFF"] }
    if $tracy { $defines ++= ["-DAMBROSIA_TRACY=ON"] }
    if $mesa { $defines ++= ["-DAMBROSIA_SWITCH_MESA=ON"] }
    let data = game-data
    if $preset != "switch" {
        $defines ++= [$"-DAMBROSIA_DATA_TESTS=(if $data == null { 'OFF' } else { 'ON' })"]
    }
    $defines ++= $given.args

    if $clean and ($ROOT | path join $dir | path exists) {
        print $"== removing ($dir)"
        rm -rf ($ROOT | path join $dir)
    }
    let config = if $preset == "debug" { "Debug" } else { "Release" }
    # TSan's runtime cannot lay out its shadow memory where a kernel with 32 bits of mmap
    # randomisation (vm.mmap_rnd_bits, as WSL's has) puts the program, and every test then stops
    # at once on "unexpected memory mapping". Its tests run with address randomisation off.
    let tsan_tests = $test and $preset == "tsan"
    let rt = if $containered {
        # setarch -R needs personality(), which container runtimes' default seccomp profiles
        # refuse. Podman and Docker can lift it for the one container; wslc cannot.
        if $tsan_tests and ($runtime | default $env.CONTAINER_RUNTIME?) == "wslc" {
            fail "the tsan tests need address randomisation off, which wslc's containers refuse; use --runtime docker or --runtime podman"
        }
        find-runtime $runtime --skip (if $tsan_tests { ["wslc"] } else { [] })
    } else { null }
    let memory = runtime-memory $rt
    let jobs = if $test { $jobs | default (test-jobs $preset $memory) } else { 0 }
    let ctest = ["ctest" "--test-dir" $dir "-C" $config "--output-on-failure" $"-j($jobs)"]
    let steps = [
        (["cmake" "--preset" $preset "-B" $dir] ++ $defines)
        ["cmake" "--build" $dir "--config" $config]
        ...(if $test { [(if $tsan_tests { ["setarch" "-R"] ++ $ctest } else { $ctest })] } else { [] })
    ]
    if $test { print $"== ($jobs) tests at a time" }
    print $"== ($dir)"
    if $containered {
        let options = if $tsan_tests { ["--security-opt=seccomp=unconfined"] } else { [] }
        let images = if $preset == "switch" {
            [{tag: $SWITCH_IMAGE, file: "Containerfile.switch"}] ++ (if $mesa { [{tag: $MESA_IMAGE, file: "Containerfile.switch-mesa"}] } else { [] })
        } else {
            [{tag: $DESKTOP_IMAGE, file: "Containerfile"}]
        }
        for image in $images { build-image $rt $image.tag $image.file }
        run-in-container $rt ($images | last | get tag) $steps $data $memory ($preset != "switch") $options
    } else {
        with-env (host-env) {
            for step in $steps { exec-checked $step }
        }
    }
}

# Run a program from a build folder, passing on the rest of the arguments.
def --wrapped "main run" [
    program: string        # ambrosia, dented or another program in the folder's bin
    --preset: string = "release"
    --container            # from the container build's folder (default with AMBROSIA_CONTAINER=1)
    --no-container         # from this machine's build folder
    --dir: string          # another build folder, such as build/release-lto
    ...args: string        # passed to the program
] {
    if (wants-help ([$program] ++ $args)) { return (help main run) }
    cd $ROOT
    let dir = $dir | default (build-dir $preset (use-container $preset $container $no_container) {})
    let bin = $ROOT | path join $dir "bin"
    let suffix = if $nu.os-info.name == "windows" { ".exe" } else { "" }
    # Visual Studio puts each configuration in a folder of its own under bin.
    let found = ["" Release Debug RelWithDebInfo]
        | each {|config| $bin | path join $config $"($program)($suffix)" }
        | where {|candidate| $candidate | path exists }
    if ($found | is-empty) {
        fail $"($program) is not built in ($dir); run `nu x.nu build ($preset)` first"
    }
    exec-checked ([($found | first)] ++ $args)
}

# Format the C++ sources with clang-format 22 or newer ($CLANG_FORMAT, or clang-format on the PATH).
def "main format" [
    --check   # fail if clang-format would change any file, changing none
] {
    let clang_format = $env.CLANG_FORMAT? | default "clang-format"
    if (which $clang_format | is-empty) {
        fail $"($clang_format) not found; run `mise install`, or set CLANG_FORMAT to clang-format 22 or newer"
    }
    let version = (^$clang_format --version | parse --regex 'version (?<major>\d+)' | get major.0? | default "0" | into int)
    if $version < 22 {
        fail $"($clang_format) is version ($version), older than 22"
    }
    # glob reads a backslash as an escape, so on Windows the root's separators become slashes.
    let root = $ROOT | str replace --all '\' '/'
    let files = [
        ...(glob $"($root)/src/**/*.{cc,h}")
        ...(glob $"($root)/tests/**/*.cc")
        ...(glob $"($root)/scripts/dll-check/*.cpp")
    ] | sort
    let mode = if $check { ["--dry-run" "--Werror"] } else { ["-i"] }
    # Windows limits a command line to 32767 characters, so the files go in batches.
    let failed = $files | chunks 150 | each {|batch|
        run-external $clang_format ...$mode ...$batch
        $env.LAST_EXIT_CODE != 0
    } | any {|failure| $failure }
    if $failed {
        fail (if $check { "format the files above with `nu x.nu format`" } else { "clang-format failed" })
    }
}

# Run the Switch runner, scripts/switch-runner.py, with the rest of the arguments.
def --wrapped "main runner" [...args: string] {
    cd $ROOT
    exec-checked (["python" ($ROOT | path join "scripts" "switch-runner.py")] ++ $args)
}

# Helpers.

# A wrapped command passes --help on with the other arguments.
def wants-help [args: list] {
    $args | any {|arg| $arg in ["--help" "-h"] }
}

def fail [message: string] {
    print -e $"x: ($message)"
    exit 1
}

# Runs a command, stopping the tool with its exit code when it fails.
def exec-checked [command: list<string>] {
    run-external ($command | first) ...($command | skip 1)
    if $env.LAST_EXIT_CODE != 0 {
        exit $env.LAST_EXIT_CODE
    }
}

def truthy [value] {
    ($value | default "" | str lowercase) in ["1" "on" "yes" "true"]
}

# The Switch build always runs in its container. MinGW does where its cross compiler is missing.
def use-container [preset: string, container: bool, no_container: bool] {
    if $preset == "switch" { return true }
    if $container { return true }
    if $no_container { return false }
    if (truthy $env.AMBROSIA_CONTAINER?) { return true }
    $preset == "mingw" and (which x86_64-w64-mingw32-g++-posix | is-empty)
}

# build/<preset>, container- in front for a container build (not switch, whose builds all are), and
# a suffix per feature, so each combination keeps a folder of its own.
def build-dir [preset: string, containered: bool, features: record] {
    let prefix = if $containered and $preset != "switch" { "container-" } else { "" }
    let suffix = [
        (if ($features.lto? | default false) { "-lto" })
        (if ($features.no_lto? | default false) { "-no-lto" })
        (if ($features.tracy? | default false) { "-tracy" })
        (if ($features.mesa? | default false) { "-mesa" })
    ] | compact | str join
    $"build/($prefix)($preset)($suffix)"
}

# The game data the tests are given: $DEUSEX_GAME_DATA, else game-data in the checkout, else none.
def game-data [] {
    let given = $env.DEUSEX_GAME_DATA?
    if $given != null and ($given | path exists) { return ($given | path expand) }
    let local = $ROOT | path join "game-data"
    if ($local | path exists) { $local } else { null }
}

# How many tests run at once: one per CPU thread, fewer for a sanitizer preset, by the memory
# each test takes, with three quarters of the machine's memory to share.
def test-jobs [preset: string, memory: filesize] {
    let cpus = sys cpu | length
    let per_test = $TEST_GB | get -o $preset
    if $per_test == null { return $cpus }
    let fit = ($memory * 0.75 / (1GiB * $per_test)) | math floor | into int
    [$cpus ([$fit 1] | math max)] | math min
}

# The memory a build and its tests have: this machine's, or the less of it and Podman's or Docker's
# own, which on Windows and macOS is a virtual machine's and may be much less. wslc reports none.
def runtime-memory [rt] {
    let host = (sys mem).total
    let query = if $rt == "docker" {
        ["docker" "info" "--format" "{{.MemTotal}}"]
    } else if $rt == "podman" {
        ["podman" "info" "--format" "{{.Host.MemTotal}}"]
    } else {
        return $host
    }
    let answer = do { ^($query | first) ...($query | skip 1) } | complete
    let bytes = try { $answer.stdout | str trim | into int } catch { 0 }
    if $answer.exit_code != 0 or $bytes <= 0 { return $host }
    [$host ($bytes | into filesize)] | math min
}

# Ninja and a job per CPU thread on this machine, unless set already; Visual Studio on Windows.
def host-env [] {
    let generator = if $nu.os-info.name == "windows" { {} } else { {CMAKE_GENERATOR: ($env.CMAKE_GENERATOR? | default "Ninja")} }
    $generator | merge {CMAKE_BUILD_PARALLEL_LEVEL: ($env.CMAKE_BUILD_PARALLEL_LEVEL? | default (sys cpu | length | into string))}
}

# The first container runtime that is installed and answers, in the order docker, wslc, podman on
# Windows and wslc, podman, docker elsewhere, or the one named by --runtime or $CONTAINER_RUNTIME.
# --skip leaves runtimes out of that order.
def find-runtime [wanted?: string, --skip: list<string> = []] {
    let wanted = $wanted | default $env.CONTAINER_RUNTIME?
    let order = if $nu.os-info.name == "windows" { $WINDOWS_RUNTIMES } else { $RUNTIMES }
    let names = if $wanted != null { [$wanted] } else { $order | where {|name| $name not-in $skip } }
    let installed = $names | where {|name| which $name | is-not-empty }
    if ($installed | is-empty) {
        if $wanted != null { fail $"($wanted) is not installed" }
        fail "no container runtime found; install Podman or Docker (or WSL, which brings wslc, on Windows)"
    }
    # `info` fails when the runtime is installed but its engine is not running.
    let answering = $installed | where {|name| (do { ^$name info } | complete).exit_code == 0 }
    if ($answering | is-empty) {
        fail $"($installed | str join ', ') found but not running; start it, or pick another with --runtime or CONTAINER_RUNTIME"
    }
    $answering | first
}

# SELinux hosts (Fedora and its derivatives) refuse a container the home folder unless labelling is
# off; relabelling instead would change the files' labels on the host. wslc has no such option.
def security-options [rt: string] {
    if $rt in [podman docker] { ["--security-opt=label=disable"] } else { [] }
}

# Builds an image from container/<file>; later runs reuse its cached layers.
def build-image [rt: string, tag: string, file: string] {
    print $"== image ($tag) \(($rt)\)"
    let context = $ROOT | path join "container"
    exec-checked ([$rt "build"] ++ (security-options $rt) ++ ["-t" $tag "-f" ($context | path join $file) $context])
}

# Quotes an argument for sh.
def sh-quote [arg: string] {
    "'" + ($arg | str replace --all "'" "'\\''") + "'"
}

# Runs the steps in the image, with the checkout mounted where it is on the host (at /src on
# Windows) and, on Linux, as your user, so the build folder is yours and its programs run on the
# host. The container may use three quarters of the memory, swap included, so a build or test run
# that outgrows it is stopped instead of the desktop; wslc has no such limit (WSL's own is set in
# .wslconfig). Compiler output is cached in a volume that outlives the containers.
def run-in-container [rt: string, image: string, steps: list, data, memory: filesize, ccache: bool, options: list = []] {
    let windows = $nu.os-info.name == "windows"
    let workdir = if $windows { "/src" } else { $ROOT }
    let user = if $windows or $rt == "wslc" {
        []
    } else if $rt == "podman" {
        ["--userns=keep-id"]
    } else {
        ["--user" $"(^id -u | str trim):(^id -g | str trim)"]
    }
    mut mounts = ["-v" $"($ROOT):($workdir)"]
    if $ccache { $mounts ++= ["-v" $"($CCACHE_VOLUME):/ccache"] }
    mut env_args = ["-e" "CMAKE_GENERATOR=Ninja" "-e" $"CMAKE_BUILD_PARALLEL_LEVEL=(sys cpu | length)" "-e" "HOME=/tmp"]
    if $data != null {
        let inside = if ($data | str starts-with $ROOT) {
            $workdir + ($data | str substring ($ROOT | str length).. | str replace --all '\' '/')
        } else {
            let at = if $windows { "/game-data" } else { $data }
            $mounts ++= ["-v" $"($data):($at):ro"]
            $at
        }
        $env_args ++= ["-e" $"DEUSEX_GAME_DATA=($inside)"]
    }
    let limits = if $rt in [podman docker] {
        let cap = $"(($memory * 0.75 / 1MiB) | math floor | into int)m"
        [$"--memory=($cap)" $"--memory-swap=($cap)"]
    } else { [] }
    let script = $steps | each {|step| $step | each {|arg| sh-quote $arg } | str join " " } | str join " && "
    exec-checked ([$rt "run" "--rm"] ++ (security-options $rt) ++ $options ++ $limits ++ $user ++ $mounts ++ $env_args ++ ["-w" $workdir $image "sh" "-euc" $script])
}
