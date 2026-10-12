# Repository layout: `include/` and `build/`

Since 2026-10-11 the imported / ST-generated headers of the two CPU2 images live in `include/` and every build output lives in `build/`.
Nothing about the firmware changed: the satellite (default, `CUBUS=T`) and ground-station binaries are **byte-identical** to the ones built
before the move (sha256 `ac12bf1a…` 50,940 B, `340970bb…` 50,692 B, `d13fbd67…` 42,176 B).

## `build/`

```
build/
  satellite/        satellite CPU2 image: satellite.elf  satellite.bin  satellite.map  .opts  and the objects (.o / .d) in app/ board/ boot/ common/ mission/ radio/ system/ uart/
  ground_station/   ground-station CPU2 image: same structure
```

* Set by `BUILD := $(REPO)/build/$(TARGET)` in `cpu2/common/cpu2.mk`; `make BUILD=<dir>` still overrides it (the CUBUS pin test builds into a temp dir).
* `build/` is git-ignored (`/build/` in `.gitignore`). `make clean` (root, TUI, or inside `cpu2/<image>`) removes only the folder of the cleaned image.
* `build/<image>/.opts` is the option stamp: when AUTH / DEBUG / DIGI / RTC / CUBUS / LOG / FRONTEND change, the objects are deleted and rebuilt.
* `generated/` (`s2s2_config.h`, `s2s2.mk`, `secrets.h`) is deliberately **not** inside `build/`: `secrets.h` holds the AUTH key shared by both
  boards and must survive a `make clean`.
* The CPU1 (NuttX) build still goes to `nuttx/` (its own tree); it is not part of `build/`.
* Tools that read the outputs: `tools/config/s2s2_core.py` (`TARGETS[...]["bin"/"elf"]`, relative to the repo root) and the testbench
  (`tb/util.py` `SAT_BUILD` / `GS_BUILD`).

## `include/`

`include/board.h` is the NuttX board header of CPU1 and stays where NuttX expects it. The CPU2 headers are in sub-folders, so nothing is mixed with it:

| Folder | What | Origin |
|---|---|---|
| `include/cmsis/` | `Core/Include/*` (core_cm0plus.h, cmsis_gcc.h, ...) and `Device/ST/STM32WLxx/Include/*` (stm32wl55xx.h, ...) | ARM CMSIS + ST device pack |
| `include/stm32wlxx_hal/` | the HAL / LL headers (`stm32wlxx_hal_*.h`, `stm32wlxx_ll_*.h`) | ST HAL (was `vendor/stm32wlxx_hal/Inc`) |
| `include/st_radio/` | SX126x driver and radio middleware headers (radio.h, radio_driver.h, sx126x*.h, stm32_timer.h, ...) | ST SubGHz_Phy middleware |
| `include/st_utilities/` | ST utilities (sequencer / trace / mailbox mux / systime headers) | ST utilities |
| `include/st_system/` | ST-generated system headers: `main.h`, `stm32wlxx_hal_conf.h`, `sys_conf.h`, `utilities_conf.h`, `timer.h`, `rtc.h`, `stm32_lpm.h`, `stm32_seq.h`, `timer_if.h`, `subghz.h`, ... | CubeMX output, unmodified |
| `include/st_bsp/` | `stm32wlxx_nucleo.h`, `stm32wlxx_nucleo_errno.h` | ST Nucleo BSP |
| `include/satellite/` | board glue + mailbox headers of the satellite image that are unmodified ST / Semtech files (`platform.h`, `radio_conf.h`, `stm32wlxx_nucleo_*.h`, `mbmux*.h`, `features_info.h`, `lr_fhss_v1_base_types.h`) | ST / Semtech, per image because the two images carry different copies |
| `include/ground_station/` | same for the ground station (`platform.h`, `radio_board_if.h`, `radio_conf.h`, `stm32wlxx_nucleo_*.h`, `lr_fhss_v1_base_types.h`) | ST / Semtech |

The exact old -> new path of every moved file is in `docs/include_moves.txt` (186 headers).

### How the split was decided

A header was moved only when it is **not our code**: it carries the STMicroelectronics (or Semtech) licence banner **and** is byte-identical to the
copy in the reference firmware we imported (`PrmBRana/GMSK_CW_OBC_COM_test`, `dummy-camera-live`). Everything we wrote or edited stays next to its sources:

* ours: `cpu2/common/{protocol,digipeater,ipc,system/include/ll_port.h}`, `cpu2/<image>/{app,mission,uart,ipc/obc_shared.h,ipc/saf_shared.h,...}`
* ST skeletons that **we edited**: `cpu2/satellite/board/radio_board_if.h` (5 V rail macros, CUBUS profile) and the `mission/config.h`, `uart/uart_debug.h`, `app/*_app.h` headers
* team code that is identical to the reference but not ST: `Sring_buffer.h`, `ax25.h`, `crc16.h`, `g3ruh.h`, `camera_image.h`, `radio_app.h`, `ipcc_test.h`

ST **sources** (`.c`) are not headers and stay in `cpu2/common/vendor/` (`st_radio/radio_fw.c`, `st_radio/stm32_timer.c`, `stm32wlxx_hal/Src/*.c`).

### Search order (matters because some file names exist twice)

`cpu2/common/cpu2.mk` keeps the original order of the `-I` list, with the new folders in the places of the old ones:

```
-I. -Iapp -Imission -Iboard -Isystem -Iradio -Iuart -Iipc          # the image's own directories (our headers win)
-Iinclude/<image>                                                 # satellite / ground_station: per-image ST glue
-Igenerated                                                       # s2s2_config.h, secrets.h
-Icpu2/common  -Icommon/protocol(/ax25,/g3ruh)  -Icommon/system/include  -Icommon/system  -Icommon/digipeater
-Iinclude/st_system  -Iinclude/st_radio  -Iinclude/stm32wlxx_hal  -Iinclude/st_bsp  -Icommon/ipc
-Iinclude/st_utilities
-Iinclude/cmsis/Device/ST/STM32WLxx/Include  -Iinclude/cmsis/Core/Include
```

Names that exist in more than one place (`features_info.h`, `mbmux*.h`, `sys_debug.h`, `stm32wlxx_nucleo_conf.h`, `sys_app.h`, ...) resolve exactly as before
because the folders were moved as groups and the order was kept. The proof is the byte-identical binaries above.

### Adding a header

* your own header: next to its `.c` (or in `cpu2/common/<area>/` when both images use it); no change to the Makefiles
* a new ST / generated header: drop it into the matching `include/<group>/` folder; a new group needs one `-I$(INC)/<group>` line in `cpu2/common/cpu2.mk`

### Caveat

`include/` is also the NuttX board include directory (`nuttx/include/arch/board` points at the board's `include/`). The CPU2 folders add about 10 MB of
headers there; they are never referenced by the NuttX build, but do not run tools that scan `include/` recursively as if it contained only `board.h`.
