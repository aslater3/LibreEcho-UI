#ifndef LE_INHERITED_FDS_H
#define LE_INHERITED_FDS_H

/* Close descriptors inherited by a forked request worker, except its stream. */
void le_close_inherited_fds(int keep_fd);

#endif
