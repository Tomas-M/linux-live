xrdb -merge ~/.Xresources
compton -D 5 -c -f -l -2 -t -2 -r 0 -o 1 -z --shadow-exclude 'bounding_shaped' &
startfluxbox
