#ifndef __CLIENT_H__
#define __CLIENT_H__

void clientInit();
void clientRecv(int sfd, NetClientNode *client);
void clientDrop(NetClientNode *client);

#endif  /* __CLIENT_H__ */
