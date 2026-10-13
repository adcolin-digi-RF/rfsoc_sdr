# RFSoC_COM reproducible Vitis project

This repository was distilled from the supplied `vitis(2).zip` workspace and keeps only the pieces needed to reconstruct **RFSoC_COM**.

## Baseline

- Tool: **Vitis 2022.2**
- Platform: `ZCU111_PLDDR_DMA`
- Hardware: `hardware/design_1_wrapper.xsa`
- Processor: `psu_cortexa53_0`
- OS: standalone, AArch64 / 64-bit
- Application domain: `standalone_psu_cortexa53_1`
- Application: `RFSoC_COM`
- System project: `RFSoC_COM_system`
- Application template: Empty Application (C)

## Libraries/settings intentionally reproduced

The rebuild script explicitly installs these BSP libraries:

- `lwip211` 1.8 — TCP/IP stack used by `eth_ddr.c` and `platform_zynqmp.c`
- `xilffs` 4.8 — present in the original RFSoC_COM BSP/domain
- `libmetal` 2.4 — required by the RFDC sources and present in the original linked BSP artifacts

The application also explicitly uses:

- math library: `-lm`
- A53 large code model: `-mcmodel=large` for compile/link
- custom linker script: `src/lscript.ld`

Do **not** replace `src/lscript.ld` with a newly generated default linker script. The baseline script defines application/DMA memory regions and the external MIG DDR mapping.

## Rebuild on Windows

Install/use Vitis **2022.2**, then run:

```bat
scripts\build_project.bat
```

If `xsct.bat` is not on PATH, the wrapper also checks the common install path:

```text
C:\Xilinx\Vitis\2022.2\bin\xsct.bat
```

The generated workspace is:

```text
build\vitis_workspace
```

Open that directory as the Vitis workspace after the script completes.

## Rebuild from an XSCT shell directly

```text
xsct scripts/create_vitis_project.tcl
```

Or choose a different generated workspace:

```text
xsct scripts/create_vitis_project.tcl C:/work/my_rfsoc_workspace
```

## What is version controlled

Track these directories/files in Git:

```text
hardware/design_1_wrapper.xsa
src/
scripts/
README.md
.gitignore
```

Do not commit `build/`; it is regenerated from the XSA + source + script.

## TCP path preserved

The source tree is copied intact from the supplied project. In particular, `eth_ddr.c` remains part of the application, so the existing lwIP TCP implementation and its server configuration are preserved rather than replaced with a Vitis example application.

## Notes on exact reproducibility

Vitis BSP/driver versions are release-dependent. Use **Vitis 2022.2** for this baseline. The XSA is also retained in the repository because the BSP device/driver configuration (RFDC, AXI DMA, GEM, DDR/MIG, etc.) is generated from that hardware handoff.
