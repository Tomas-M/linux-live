# ~/.bashrc: executed by bash(1) for non-login shells.

# Note: PS1 and umask are already set in /etc/profile. You should not
# need this unless you want different defaults for root.
# PS1='${debian_chroot:+($debian_chroot)}\h:\w\$ '
# umask 022

export EDITOR=mcedit
export LS_OPTIONS='--color=auto'
eval "`dircolors`"

ls() { /bin/ls $LS_OPTIONS "$@"; }
ll() { /bin/ls $LS_OPTIONS -l "$@"; }
l() { /bin/ls $LS_OPTIONS -lA "$@"; }

apt-get()
{
   if [ -e /var/cache/apt/pkgcache.bin ]; then
      /usr/bin/apt-get "$@"
   else
      /usr/bin/apt-get update
      /usr/bin/apt-get "$@"
   fi
}

apt()
{
   if [ -e /var/cache/apt/pkgcache.bin ]; then
      /usr/bin/apt "$@"
   else
      /usr/bin/apt update
      /usr/bin/apt "$@"
   fi
}

export -f ls
export -f ll
export -f l
export -f apt-get
export -f apt
