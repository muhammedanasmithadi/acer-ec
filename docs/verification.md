# Verification matrix

How to prove the tree is good. Rootless gates run anywhere;
live gates need the target laptop plus root.

## Rootless (CI-safe)

```bash
make modules          # clean kbuild, 0 errors (try W=1 and W=2 too)
make check            # checkpatch --strict 0e/0w on all src/*.c + shellcheck clean
bash -n install.sh uninstall.sh src/acer-ec.sh
git status --short    # clean after build (outputs are gitignored)
```

`make check` exits nonzero on any finding — that is the gate working,
not a broken target.

## Live (root on the A715-79G)

```bash
sudo ./install.sh     # DKMS add/build/install, configs, CLI, modprobe
dkms status | grep acer-ec                    # 1.0 installed
cat /sys/module/acer_fanctl/srcversion        # equals modinfo -F srcversion
cat /sys/kernel/acer_fanctl/all               #dut/rpm/temp/profile sane
sudo cat /sys/kernel/debug/acer_ec/batt      #soc/pack/charge/cutoff sane vs sysfs
sensors | grep -A4 acer_ec                    # fans + temp visible
# 4 = channel mask 0b1000 -> writes 0xFF to EC register 0x04
echo 4 | sudo tee /sys/kernel/acer_fanctl/profile && cat /sys/kernel/acer_fanctl/profile
sudo cat /sys/kernel/debug/acer_ec/reg8_CE    # matches dut1 in all
readlink /sys/bus/wmi/devices/*ABBC0F6C*/driver   # acer_wmi_extras
sudo acer-ec profile 2                 # round-trip OK (channel mask, not a profile)
sudo ./uninstall.sh   # no dkms entry, no sysfs, no configs, no /usr/local/bin/acer-ec
sudo ./install.sh     # back to mask 4 (the configured default), resting state
```

## Known firmware behaviors (not bugs)

- `fan2_duty_set` stores return success (`tee=0`) but the EC may not
  reflect the write in `fan2_duty` — the channel-1 preservation
  (RMW under `fanctl_lock`) is the guaranteed property.
- There is deliberately no `fan1_duty_set`: SCMD 0x68 byte0 writes latch
  the CPU fan off (verified live, see git history).
- `profile` does not select a fan profile. SCMD 0x69 is a 4-bit channel
  mask writing `0xFF` to one EC register per set bit (`dsdt.dsl:97197`).
  The `quiet`/`balanced`/`performance`/`gaming` labels are legacy names
  kept for compatibility; mask 1 pins fan 1 to full duty.
