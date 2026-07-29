#include "flags.h"

raft_flags flagsSet(raft_flags in, raft_flags flags)
{
	return in | flags;
}

raft_flags flagsClear(raft_flags in, raft_flags flags)
{
	return in & (~flags);
}

bool flagsIsSet(raft_flags in, raft_flags flag)
{
	return (bool)(in & flag);
}
