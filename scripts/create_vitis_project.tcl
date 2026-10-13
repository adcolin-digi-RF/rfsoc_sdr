# Recreate the RFSoC_COM Vitis 2022.2 workspace from source + XSA.
# Run with: xsct scripts/create_vitis_project.tcl ?workspace?

set script_dir [file dirname [file normalize [info script]]]
set repo_root  [file normalize [file join $script_dir ..]]

if {$argc >= 1} {
    set workspace [file normalize [lindex $argv 0]]
} elseif {$tcl_platform(platform) eq "windows" && [info exists ::env(USERPROFILE)]} {
    # Keep Windows build output short; Vitis 2022.2/libmetal uses an old CMake
    # that can still hit legacy MAX_PATH limits in deeply nested BSP builds.
    set workspace [file normalize [file join $::env(USERPROFILE) vws rfsoc_com]]
} else {
    set workspace [file normalize [file join $repo_root build vitis_workspace]]
}

set xsa     [file join $repo_root hardware design_1_wrapper.xsa]
set src_dir [file join $repo_root src]
set platform_name ZCU111_PLDDR_DMA
set domain_name   standalone_psu_cortexa53_1
set app_name      RFSoC_COM
set proc_name     psu_cortexa53_0

proc require_file {p} {
    if {![file exists $p]} {
        error "Required file not found: $p"
    }
}

require_file $xsa
require_file [file join $src_dir main.c]
require_file [file join $src_dir lscript.ld]

puts "============================================================"
puts "RFSoC_COM reproducible Vitis build"
puts "Workspace : $workspace"
puts "XSA       : $xsa"
puts "Sources   : $src_dir"
puts "============================================================"

# This path is dedicated build output. Recreate it to prevent stale Eclipse/Vitis metadata.
if {[file exists $workspace]} {
    puts "Removing previous generated workspace: $workspace"
    file delete -force $workspace
}
file mkdir $workspace
setws $workspace

# -----------------------------------------------------------------------------
# Platform + ZynqMP boot components.
#
# The original ZCU111_PLDDR_DMA/platform.tcl created the platform with:
#   -proc psu_cortexa53_0 -os standalone -arch 64-bit
#   -fsbl-target psu_cortexa53_0
# This generates the ZynqMP FSBL and PMUFW used by RFSoC_COM_system when it
# creates BOOT.BIN.  Earlier revisions omitted these to avoid the Windows
# CMake/MAX_PATH failure.  The generated workspace is now intentionally short
# (%USERPROFILE%\vws\rfsoc_com), so the boot BSPs can be restored.
# -----------------------------------------------------------------------------
platform create \
    -name $platform_name \
    -hw $xsa \
    -proc $proc_name \
    -os standalone \
    -arch 64-bit \
    -fsbl-target $proc_name

platform write
platform active $platform_name

# Dedicated application domain matching the baseline RFSoC_COM.prj.
domain create \
    -name $domain_name \
    -display-name $domain_name \
    -os standalone \
    -proc $proc_name \
    -runtime cpp \
    -arch 64-bit \
    -support-app empty_application

platform generate -domains $domain_name
domain active $domain_name

# BSP libraries required by this application.
# lwip211 -> eth_ddr.c/platform_zynqmp.c TCP stack
# xilffs  -> retained from the baseline domain
# libmetal -> RFDC sources include/use libmetal and the baseline ELF links libmetal.a
bsp setlib -name lwip211 -ver 1.8
bsp setlib -name xilffs  -ver 4.8
bsp setlib -name libmetal -ver 2.4
bsp write
bsp reload
catch {bsp regenerate}

platform generate -domains $domain_name
platform write

# -----------------------------------------------------------------------------
# Application + system project.
# Creating without -sysproj gives the baseline RFSoC_COM_system automatically.
# The platform above includes FSBL + PMUFW, so the system project can also be
# built to produce a boot image.
# -----------------------------------------------------------------------------
app create \
    -name $app_name \
    -platform $platform_name \
    -domain $domain_name \
    -template {Empty Application(C)} \
    -lang c

# Copy all baseline sources, including the custom linker script.
importsources -name $app_name -path $src_dir -linker-script

# Match the baseline Debug build settings.
app config -name $app_name -set build-config Debug
app config -name $app_name -add compiler-misc {-mcmodel=large}
app config -name $app_name -add linker-misc {-mcmodel=large}
app config -name $app_name -add libraries {m}
app config -name $app_name -set linker-script {../src/lscript.ld}

puts "Building application..."
app build -name $app_name

puts "Building system project / BOOT.BIN..."
sysproj build -name ${app_name}_system

puts ""
puts "============================================================"
puts "DONE"
puts "Open this workspace in Vitis 2022.2:"
puts "  $workspace"
puts "Application ELF:"
puts "  [file join $workspace $app_name Debug ${app_name}.elf]"
puts "System build directory:"
puts "  [file join $workspace ${app_name}_system Debug]"
puts "============================================================"
