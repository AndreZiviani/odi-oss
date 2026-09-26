/*
 * Device overrides for dropbear.
 *
 * dropbear's supported override point: default_options.h guards every value
 * with #ifndef, so defining a symbol here wins without patching upstream
 * source. Nothing in this file is a workaround for the toolchain -- the libc
 * problems are handled by ./configure flags in build.sh.
 */

/*
 * DEFAULT_PATH is what a NON-INTERACTIVE session gets. `ssh host cmd` is not a
 * login shell and never reads /etc/profile, so profile cannot fix it. Stock
 * dropbear hands out /usr/sbin:/usr/bin:/sbin:/bin, which omits /etc/scripts
 * -- and that is why every scripted command on the vendor image has to spell
 * out /etc/scripts/flash. Boot-time processes already inherit
 * /sbin:/usr/sbin:/bin:/usr/bin:/etc/scripts, so this only gives remote
 * sessions the PATH the device already uses for itself.
 */
#define DEFAULT_PATH "/sbin:/usr/sbin:/bin:/usr/bin:/etc/scripts"
#define DEFAULT_ROOT_PATH "/sbin:/usr/sbin:/bin:/usr/bin:/etc/scripts"
