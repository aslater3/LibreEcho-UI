#ifndef LE_MDNS_LEASE_H
#define LE_MDNS_LEASE_H

/* A single fixed Wyoming owner. The supervisor owns the directory fd.
 * Return values describe local file state, never confirmed publication. */
struct le_mdns_lease {
    int directory_fd;
    int owner_fd;
    unsigned long generation;
    char filename[64];
};
void le_mdns_lease_init(struct le_mdns_lease *lease, int directory_fd);
int le_mdns_lease_register(struct le_mdns_lease *lease, int owner_fd,
                           unsigned int port);
int le_mdns_lease_withdraw(struct le_mdns_lease *lease, int owner_fd);
#endif
