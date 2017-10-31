xrdb -merge ~/.Xresources
feh --bg-scale /usr/share/wallpapers/wp.jpg
compton -D 5 -c -f -l -2 -t -2 -r 0 -o 1 -z --shadow-exclude 'bounding_shaped' &

if [ -e /usr/bin/startfluxbox ]; then
   /usr/bin/startfluxbox
else
   if [ -e /usr/bin/blackbox ]; then
      /usr/bin/blackbox
   else
      xterm
   fi
fi
