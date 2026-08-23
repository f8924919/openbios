\   QEMU specific initialization code
\
\   This program is free software; you can redistribute it and/or
\   modify it under the terms of the GNU General Public License
\   as published by the Free Software Foundation
\

include config.fs

\ ---------
\ DMA words
\ ---------

: ppc-dma-free  ( virt size -- )
  2drop
;

: ppc-dma-map-out  ( virt devaddr size -- )
  (dma-sync)
;

['] ppc-dma-free to (dma-free)
['] ppc-dma-map-out to (dma-map-out)

\ -------------------------------------------------------------
\ device-tree
\ -------------------------------------------------------------

\ Number of address cells on the root node.  PowerMac7,3 switches this
\ to 2 at runtime (see arch_of_init()); everything else keeps the
\ compile-time value.
variable root-#adr-cells
[IFDEF] CONFIG_PPC64 2 [ELSE] 1 [THEN] root-#adr-cells !

" /" find-device
\ Apple calls the root node device-tree
" device-tree" device-name
[IFDEF] CONFIG_PPC64 2 [ELSE] 1 [THEN] encode-int " #address-cells" property
1 encode-int " #size-cells" property
h# 05f5e100 encode-int " clock-frequency" property

\ Root unit addresses follow #address-cells: one cell keeps the legacy
\ bare hex form, two cells use the Apple "hi,lo" form (e.g. ht@0,f2000000).
: decode-unit ( str len -- unit.lo [unit.hi] )
  root-#adr-cells @ 2 = if
    2 parse-nhex swap
  else
    parse-hex
  then
;

: encode-unit ( unit.lo [unit.hi] -- str len )
  root-#adr-cells @ 2 = if
    pocket tohexstr
    " ," pocket tmpstrcat >r
    rot pocket tohexstr r> tmpstrcat drop
  else
    pocket tohexstr
  then
;

	: dma-sync
	  (dma-sync)
	;

	: dma-alloc
	  (dma-alloc)
	;

	: dma-free
	  (dma-free)
	;

	: dma-map-in
	  (dma-map-in)
	;

	: dma-map-out
	  (dma-map-out)
	;

new-device
	" cpus" device-name
	1 encode-int " #address-cells" property
	0 encode-int " #size-cells" property
	external

	: encode-unit ( unit -- str len )
		pocket tohexstr
	;

	: decode-unit ( str len -- unit )
		parse-hex
	;

finish-device

new-device
	" memory" device-name
	" memory" device-type
	external
	: open true ;
	: close ;
finish-device

new-device
	" rom" device-name
	h# ff800000 encode-int 0 encode-int encode+ " reg" property
	1 encode-int " #address-cells" property
	h# ff800000 encode-int h# 800000 encode-int encode+
	h# ff800000 encode-int encode+ " ranges" property
finish-device

\ -------------------------------------------------------------
\ /packages
\ -------------------------------------------------------------

" /packages" find-device

	" packages" device-name
	external
	\ allow packages to be opened with open-dev
	: open true ;
	: close ;

\ /packages/terminal-emulator
new-device
	" terminal-emulator" device-name
	external
	: open true ;
	: close ;
	\ : write ( addr len -- actual )
	\	dup -rot type
	\ ;
finish-device

\ -------------------------------------------------------------
\ The END
\ -------------------------------------------------------------
device-end
