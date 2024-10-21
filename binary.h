#ifndef __BINARY_H__
#define __BINARY_H__

#define LEN_BINARYID 11

void binaryInit();
bool binaryRecv(int sfd, NetBinaryNode *client);
void binaryDrop(NetBinaryNode *client);

#endif  /* __BINARY_H__ */
