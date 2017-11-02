xrdb -merge ~/.Xresources
compton --sw-opti -e 0.9 --opacity-rule '70:WM_WINDOW_ROLE@:s *?= "fluxbox-toolbar"' --no-fading-destroyed-argb -D 5 -c -f -l -2 -t -2 -r 0 -o 1 -z --shadow-exclude 'bounding_shaped' &
startfluxbox
