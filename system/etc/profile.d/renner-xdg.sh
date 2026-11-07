# renner.dev OS: product defaults in the image (see /usr/lib/environment.d/40-renner-xdg.conf),
# also for shells and programs started outside the desktop session.
case ":${XDG_CONFIG_DIRS:-/etc/xdg}:" in
    *:/usr/share/renner-os/xdg:*) ;;
    *) export XDG_CONFIG_DIRS="${XDG_CONFIG_DIRS:-/etc/xdg}:/usr/share/renner-os/xdg" ;;
esac
