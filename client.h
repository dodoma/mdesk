#ifndef __CLIENT_H__
#define __CLIENT_H__

void clientInit();
bool clientRecv(int sfd, NetClientNode *client);
void clientDrop(NetClientNode *client);
void clientAdd(NetClientNode *client);
NetClientNode* clientMatch(char *clientid);

#endif  /* __CLIENT_H__ */
