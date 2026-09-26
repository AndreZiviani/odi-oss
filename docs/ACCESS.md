# Getting into the stick, and moving files

How to reach a stick running this image, and how to copy things on and off
it. The stock (OEM) image differs on every point below; `docs/FLASHING.md`
covers the one moment you are on the stock image (flashing).

## Ways in

| port | what | on by default | off switch (on the jffs2 config partition) |
|---|---|---|---|
| 22 | ssh, dropbear 2026.94, user `root` | yes | `touch /etc/config/dropbear.off` |
| 80 | the web UI (confd, HTTP Basic, default `admin` / `admin`; the same port the vendor Boa uses, so one URL fits both images) | yes | `touch /etc/config/confd.off` |
| 9100 | the Prometheus exporter (metricsd) | yes | `touch /etc/config/metricsd.off` |

There is no serial console on the DFP-34X-2C2, so these are all the ways in.
Every one of them fails OPEN on purpose: a missing config file gives you the
default credential, not a locked door.

## The root password

A **release image is keys-only**: `ROOT_PW=locked` (`docs/BUILDING.md`)
leaves root with no password at all (the `/etc/passwd` field is `!`, and
dropbear runs with `-s`, refusing password logins outright — ssh does not
even prompt for one). First access is through the web UI below, whose
SSH-key admin page adds a key for root.

A **source build**, unless it also passes `ROOT_PW=locked`, generates its
own password: `out/image/root-password-<version>.txt` beside the tarball.
It is the ssh password for `root`, baked into `/etc/passwd` on the
read-only squashfs as a SHA-512 crypt hash (`$6$`): `passwd` on the stick
cannot change it. The build file is the source of truth; for anything you
do more than once, use keys.

## SSH keys (the way to do it)

dropbear is started with `-D /etc/config/dropbear.d`, so it reads
`/etc/config/dropbear.d/authorized_keys` -- the jffs2 partition, beside the
host key, so the keys survive a reflash of either slot. One OpenSSH line per
key, as `ssh-keygen` prints it.

Three ways to add one:

- **The web UI**: Admin tab, "SSH keys", paste the public key, Add. The list
  shows what is in the file and removes by line.
- **The API** the UI uses (HTTP Basic with the UI credential):

      curl -u admin:admin -X POST --data-urlencode "key=$(cat ~/.ssh/id_ed25519.pub)" \
           http://<stick>/api/sshkeys
      curl -u admin:admin http://<stick>/api/sshkeys        # list, with line numbers
      curl -u admin:admin -X POST -d delete=0 http://<stick>/api/sshkeys

- **By hand** over a password login:

      ssh root@<stick> 'mkdir -p /etc/config/dropbear.d; cat >> /etc/config/dropbear.d/authorized_keys; chmod 600 /etc/config/dropbear.d/authorized_keys' < ~/.ssh/id_ed25519.pub

dropbear reads the file on every login; nothing to restart. Password login
stays on beside the keys.

## Copying files: scp

The image ships dropbear's `scp` as `/bin/scp`, so plain scp works in both
directions. It speaks the **legacy scp protocol only** (dropbear has no SFTP
subsystem), and OpenSSH 9.0+ clients default to SFTP, so pass `-O`:

    scp -O odi-oss-<version>.tar root@<stick>:/tmp/           # to the stick
    scp -O root@<stick>:/var/log/omcid.log .                  # from the stick

Without `-O` a modern client fails with "subsystem request failed". The
older way still works too and needs nothing on the stick:

    ssh root@<stick> 'cat > /tmp/x.tar' < odi-oss-<version>.tar
    ssh root@<stick> 'cat /var/log/omcid.log' > omcid.log

**Watch the memory.** `/tmp` is ramfs (a link into `/var`), and RAM is all
there is: about 15 MB is available on a running stick (`MemAvailable` in
`/proc/meminfo`), and nothing written to ramfs is ever evicted. A 2.6 MB
image tarball fits, but do not leave copies behind. `echo 3 > /proc/sys/vm/drop_caches`
first if it is tight, and unpack only what you need (`fwu.sh` streams the
rest).

## What answers what

    omcli state              serial, device, ONU state, MIB sync
    omcli conn               the bridge connections omcid built
    omcli get sn             the vendor omcicli shapes (get/mib/dump verbs)
    diag                     the switch/optics CLI, batched on stdin
    diag gpon get flows      the GEM flows omcid programmed
    cat /var/log/omcid.log   every OMCI frame and every driver call this boot

The web UI's OMCI page and the exporter's `gpon_omci_services` read the same
daemon through `/bin/omcicli`, which is `omcli`.
