\ tag: misc useful functions
\ 
\ Open Firmware Startup
\ 
\ Copyright (C) 2003 Samuel Rydh
\ 
\ See the file "COPYING" for further information about
\ the copyright and warranty status of this work.
\ 

variable PREPOST-list
variable POST-list
variable SYSTEM-list
variable DIAG-list

: PREPOST-initializer ( xt -- )
  PREPOST-list list-add ,
;

: POST-initializer ( xt -- )
  POST-list list-add ,
;

: SYSTEM-initializer ( xt -- )
  SYSTEM-list list-add ,
;

: DIAG-initializer ( xt -- )
  DIAG-list list-add ,
;


\ Evaluate a string that comes from NVRAM, recovering from any exception.
\
\ The user interface protects itself with ['] interpret catch print-status,
\ but the NVRAM-sourced evaluations below used to run unprotected: a stray
\ word in nvramrc or boot-command threw past every frame, so the machine
\ never reached outer-interpreter and the user was left without a prompt to
\ repair the variable with.  The helper words exist because catch needs a
\ zero-argument execution token, whereas evaluate takes a string.
\
\ state is cleared like the client interface does, so that an unterminated
\ definition does not leave the interpreter in compile mode.

: (nvramrc-eval) ( -- )  nvramrc evaluate ;
: (boot-command-eval) ( -- )  boot-command evaluate ;

: nvram-evaluate ( xt -- )
  catch ?dup if
    0 state !
    print-status
  then
;

\ OpenFirmware entrypoint
: initialize-of ( startmem endmem -- )
  initialize-forth

  PREPOST-list begin list-get while @ execute repeat
  POST-list begin list-get while @ execute repeat
  SYSTEM-list begin list-get while @ execute repeat

  \ evaluate nvramrc script
  use-nvramrc? if
    ['] (nvramrc-eval) nvram-evaluate
  then

  \ probe-all etc.
  suppress-banner? 0= if
    probe-all
    install-console
    banner
  then

  DIAG-list begin list-get while @ execute repeat

  auto-boot? if
    ['] (boot-command-eval) nvram-evaluate
  then

  outer-interpreter
;
