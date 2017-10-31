xrdb -merge ~/.Xresources
if [ -e /usr/bin/startfluxbox ]; then
   /usr/bin/startfluxbox
else
   if [ -e /usr/bin/blackbox ]; then
      /usr/bin/blackbox
   else
      xterm
   fi
fi
