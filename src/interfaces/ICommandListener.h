#ifndef __ICOMMANDLISTENER_H__
#define __ICOMMANDLISTENER_H__


class Client;
class Command;


class ICommandListener
{
public:
    virtual ~ICommandListener() {}

    virtual void onCommand(const Command &command) = 0;
};


#endif // __ICOMMANDLISTENER_H__