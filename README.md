# chassisRgbAcerPO7-660

`chassisRgbAcerPO7-660` was tested on a PO7-660 with BIOS 1.10. It changes the
global chassis color and optional brightness through `/dev/i2c-0`. It uses the
existing Linux `i2c-dev` interface; no custom kernel driver is required. A
color-only command keeps the current brightness. Brightness values 0–10 work
on this controller. The global effect must be static. It does not address RAM
or GPU lighting.

```sh
sudo modprobe i2c-dev
cc -std=c11 -Wall -Wextra -O2 chassisRgbAcerPO7-660.c -o chassisRgbAcerPO7-660
sudo ./chassisRgbAcerPO7-660 --probe   # read-only effect and color check
sudo ./chassisRgbAcerPO7-660 FF6600      # change color, keep brightness
sudo ./chassisRgbAcerPO7-660 FF6600 5    # change color and brightness (0-10)
sudo ./chassisRgbAcerPO7-660 0000FF 5    # change brightness while keeping blue
sudo ./chassisRgbAcerPO7-660 0000FF      # restore blue, keep brightness
```

The program accepts only the PO7-660 and the observed static global effect.
It reads the existing color and brightness, sends one SMBus
block write at address `0x29`, command `0x07`, and reads the color and brightness
back. If the readback differs, it tries to restore both original values.

