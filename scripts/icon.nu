# Remakes the icon files in assets/icon from each program's source image (see assets/icon/README.md).
#
# Needs ImageMagick's `magick`, with its librsvg delegate for the game's SVG. Each size is made
# from the source at that size rather than scaled down from a smaller one. The .ico is put together
# here from PNG frames, which Windows reads from Vista on, as ImageMagick stores its 256 pixel frame
# uncompressed.

const ROOT = path self .
const ICON = $ROOT | path dirname | path join "assets" "icon"
const WINDOW_SIZES = [32 64 128 256]
const ICO_SIZES = [16 20 24 32 40 48 64 96 128 256]
const SWITCH_BACKGROUND = "#1b2428"

def main [] {
    make "ambrosia" "ambrosia.svg" --switch
    # The editor has no Switch build.
    make "dented" "dented.png"
}

def make [program: string, source: string, --switch] {
    let folder = $ICON | path join $program
    let source_path = $folder | path join $source
    let temp = mktemp -d
    # Each size is drawn once, for the .ico, and the window's sizes are kept from those.
    let frames = try {
        $ICO_SIZES | each {|size|
            let png = $temp | path join $"($size).png"
            render $source_path $size $png
            {size: $size, png: (open --raw $png)}
        }
    } catch {|err|
        rm -rf $temp
        fail $err.msg
    }
    rm -rf $temp
    write-ico $frames ($folder | path join $"($program).ico")
    for frame in ($frames | where {|frame| $frame.size in $WINDOW_SIZES}) {
        $frame.png | save --raw -f ($folder | path join $"($frame.size).png")
    }
    if $switch {
        # A JPEG has no transparency, so the icon sits on a dark background with a margin.
        exec-checked (["magick"] ++ (read-source $source_path 256) ++ ["-resize" "230x230"
            "-background" $SWITCH_BACKGROUND "-gravity" "center" "-extent" "256x256"
            "-flatten" "-quality" "92" "-strip" ($folder | path join "switch.jpg" | into string)])
    }
}

# The SVG is 512 pixels at 96 dpi; drawn at four times the size, then scaled to it. A PNG is
# scaled from its full size.
def read-source [source: path, size: int] {
    if ($source | path parse | get extension) == "svg" {
        ["-background" "none" "-density" (((96 * $size * 4) // 512) | into string) ($source | into string)]
    } else {
        [($source | into string)]
    }
}

def render [source: path, size: int, out: path] {
    exec-checked (["magick"] ++ (read-source $source $size) ++ ["-resize" $"($size)x($size)" "-depth" "8"
        "-strip" $"PNG32:($out)"])
}

# Windows reads the .ico's frames as PNGs. The 256 pixel frame has zero for its side, as its size
# does not fit in a byte.
def write-ico [frames: list, out: path] {
    let header_bytes = [
        (little-endian 0 2)
        (little-endian 1 2)
        (little-endian ($frames | length) 2)
    ] | flatten
    let header = bytes build ...$header_bytes
    mut offset = ($header | bytes length) + 16 * ($frames | length)
    mut entries = []
    for frame in $frames {
        let side = if $frame.size >= 256 { 0 } else { $frame.size }
        let entry_bytes = [
            (little-endian $side 1)
            (little-endian $side 1)
            (little-endian 0 1)
            (little-endian 0 1)
            (little-endian 1 2)
            (little-endian 32 2)
            (little-endian ($frame.png | bytes length) 4)
            (little-endian $offset 4)
        ] | flatten
        $entries ++= [(bytes build ...$entry_bytes)]
        $offset = $offset + ($frame.png | bytes length)
    }
    ([$header] ++ $entries ++ ($frames | each {|frame| $frame.png })) | bytes collect | save --raw -f $out
}

# An integer's bytes in little-endian order, the order the .ico header and entries use.
def little-endian [value: int, width: int] {
    mut rest = $value
    mut packed = []
    for _ in 0..<$width {
        $packed ++= [($rest mod 256)]
        $rest = $rest // 256
    }
    $packed
}

def fail [message: string] {
    print -e $"x: ($message)"
    exit 1
}

def exec-checked [command: list<string>] {
    run-external ($command | first) ...($command | skip 1)
    if $env.LAST_EXIT_CODE != 0 {
        exit $env.LAST_EXIT_CODE
    }
}
