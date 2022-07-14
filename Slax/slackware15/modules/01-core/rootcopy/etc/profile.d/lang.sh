#!/bin/sh
# For a list of locales which are supported by this machine, type:
#   locale -a

export LANG=en_US

# Preref UTF-8 support if available
if [ -d /usr/lib*/locale/$LANG.utf8 ]; then
   export LANG=$LANG.utf8
fi

# create localized user directories and update firefox configuration to save downloads to XDG_DOWNLOAD_DIR
if [ -x /usr/bin/xdg-user-dirs-update -a ! -r /root/.config/user-dirs.dirs ]; then
   /usr/bin/xdg-user-dirs-update
fi

# There is a problem with most locales (including en_US) is that it sorts
# ASCII letters differently. For example if a script uses regexp match [A-Z],
# it happens to match even most lowercase letters, because the actual
# order is AaBbCc...Zz. This is plainly crazy, so we'll stick with
# standard ASCII order here.
export LC_COLLATE=C
