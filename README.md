braindump
=========

braindump is a 3DS homebrew application that can dump 3DS titles, i.e. cartridge and eShop games, but also other installed content. Since it runs entirely in userland, it runs on any system version supported by the [Homebrew Launcher](https://smealum.github.io/3ds/).

The dumps created by braindump are lossy, since not all title information is accessible to userland applications. braindump compensates for that by generating fake metadata using clever heuristics. This is good enough to be able to run the dumped title in an emulator or to title contents (artwork, sounds, etc) for ROM hacking. If you require an (almost) lossless copy of your title, it's recommended to use [uncart](https://github.com/citra-emu/uncart) instead.

## Build Instructions and Setup

You need to have set up a 3DS homebrew[development environment](https://www.3dbrew.org/wiki/Setting_up_Development_Environment) including devkitARM and ctrulib. If you're having trouble building or running braindump, make sure your toolchain is up to date.

Once you've setup braindump, you can build it by calling
```
make
```
in the project root directory. If all went well, you then should have files `braindump.3dsx`, `braindump.xml`, and `braindump.smdh`. Put these onto your SD card into the directory `3ds/braindump/`.

When running braindump from the Homebrew Launcher, you will be prompted to select a "target title". Once you select a title, it will be dumped without any further confirmation to the the SD card root directory using the filename `<titleid>.cxi` (where `titleid` is a 16-digit identifier of the dumped title).

## Frequently Asked Questions

### What stuff can I dump with this?

* In general, most 3DS games should be dumpable, regardless of whether physical (cartridge) or digital (eShop).
* Recent 3DS games which use the 9.6-crypto cannot be dumped and probably won't ever be dumpable via braindump.
* Non-game content (videos, system applications, ...) should be dumpable if they have a "proper" title. Some applications (e.g. Home Menu) only provide dummy titles and hence cannot be dumped currently.
* GBA or DS games cannot be dumped, at least for now.
* Virtual Console games using software emulation are untested. Chances are they are dumpable.

If the application you're trying to dump is not supported, it will likely outright crash when trying to launch braindump. In particular, if the title doesn't boot with HANS, it likely won't work with braindump either. There currently is no way for braindump to fail more gracefully, unfortunately.

### How to use braindump for ROM hacking?
At this stage, I cannot give you full instructions on how to mod a game, but here are some quick hints:

* Launching
* Dumping the game contents using braindump on your 3DS. This will place the file `<titleid>.cxi` on your SD card.
* To extract the game content you have to extract the ExeFS and the RomFS. You can do this on a PC using [ctrtool](https://github.com/profi200/Project_CTR) with the commands `ctrtool --exefs=exefs.bin --decompresscode <titleid>.cxi` and `ctrtool --romfs=romfs.bin --decompresscode <titleid>.cxi; ctrtool --romfsdir=romfs --intype=romfs romfs.bin`, respectively.
* Game modders will be interested in the contents extracted to romfsdir. Modify whatever you like, and repack the contents using a tool like [3dstool](https://github.com/dnasdw/3dstool).
* Put the new romfs binary on your SD card. Start HANS on your 3DS and point it to the modded game, and make it replace the romfs with your new image.

### I tried this but it keeps getting stuck at "Dumping code... XYZ KiB"
### It's so slooooow.. why?!
Be patient. Dumping ExeFS may take up to 5 minutes per MiB, depending on how well the 3DS plays with your SD card (it's unfortunately not possible to show a progress bar for this, contrary to RomFS dumping). RomFS dumping should be going at roughly 1 MiB/s.

### Can I use the dumps with Citra?
Yes! Note that this has not been tested extensively though. If you come across a title which runs fine in Citra when dumped using uncart but not when dumped using braindump, please report an [issue](https://github.com/neobrain/braindump/issues).

### Will you add FTP support to dump directly over network???
Maybe. Pull requests are welcome!

### Will this break my 3DS?
It runs entirely in userspace, hence it's unlikely anything bad will happen. Of course, I cannot give you any guarantee for this though; I take no responsibility for anything that happens as a direct or indirect consequence of running this software on your 3DS.

## Credits

Creating braindump wouldn't have been possible if it hadn't been for the help and support of the 3DS community. My greatest thanks go to the whole 0xFOODDOODs 32c3 table for an awesome time, and in particular to smea for his repeated advice and patience.
