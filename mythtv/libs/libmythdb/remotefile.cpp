#include <iostream>
using namespace std;

#include <QUrl>

#include "mythdb.h"
#include "remotefile.h"
#include "decodeencode.h"
// #include "mythcontext.h"
#include "mythsocket.h"
#include "compat.h"
#include "mythverbose.h"

RemoteFile::RemoteFile(const QString &_path, bool write, bool useRA,
                       int _retries,
                       const QStringList *possibleAuxiliaryFiles) :
    path(_path),
    usereadahead(useRA),  retries(_retries),
    filesize(-1),         timeoutisfast(false),
    readposition(0),      recordernum(0),
    lock(QMutex::NonRecursive),
    controlSock(NULL),    sock(NULL),
    query("QUERY_FILETRANSFER %1"),
    writemode(write)
{
    if (writemode)
    {
        usereadahead = false;
        retries = -1;
    }
    else if (possibleAuxiliaryFiles)
        possibleauxfiles = *possibleAuxiliaryFiles;

    if (!path.isEmpty())
        Open();

    VERBOSE(VB_FILE,QString("RemoteFile(%1)").arg(path));
}

RemoteFile::~RemoteFile()
{
    Close();
    if (controlSock)
        controlSock->DownRef();
    if (sock)
        sock->DownRef();
}

MythSocket *RemoteFile::openSocket(bool control)
{
    QUrl qurl(path);
    QString dir;

    QString host = qurl.host();
    int port = qurl.port();
   
    dir = qurl.path();

    if (qurl.hasQuery())
        dir += "?" + QUrl::fromPercentEncoding(qurl.encodedQuery());
    
    if (qurl.hasFragment()) 
        dir += "#" + qurl.fragment();

    QString sgroup = qurl.userName();

    MythSocket *lsock = new MythSocket();
    QString stype = (control) ? "control socket" : "file data socket";

    QString loc_err = QString("RemoteFile::openSocket(%1), Error: ").arg(stype);

    if (port <= 0)
    {
        port = GetMythDB()->GetSettingOnHost("BackendServerPort", host).toInt();

        // if we still have no port use the default
        if (port <= 0)
            port = 6543;
    }

    if (!lsock->connect(host, port))
    {
        VERBOSE(VB_IMPORTANT, loc_err +
                QString("\n\t\t\tCould not connect to server %1:%2")
                .arg(host).arg(port));
        lsock->DownRef();
        return NULL;
    }
    
    QString hostname = GetMythDB()->GetHostName();

    QStringList strlist;

    if (control)
    {
        strlist.append( QString("ANN Playback %1 %2").arg(hostname).arg(false) );
        lsock->writeStringList(strlist);
        lsock->readStringList(strlist, true);
    }
    else
    {
        strlist.append( QString("ANN FileTransfer %1 %2 %3 %4")
            .arg(hostname).arg(writemode).arg(usereadahead).arg(retries) );
        strlist << QString("%1").arg(dir);
        strlist << sgroup;

        QStringList::const_iterator it = possibleauxfiles.begin();
        for (; it != possibleauxfiles.end(); ++it)
            strlist << *it;

        lsock->writeStringList(strlist);
        lsock->readStringList(strlist, true);

        if (strlist.size() >= 4)
        {
            it = strlist.begin(); ++it;
            recordernum = (*it).toInt(); ++it;
            filesize = decodeLongLong(strlist, it);
            for (; it != strlist.end(); ++it)
                auxfiles << *it;
        }
        else if (0 < strlist.size() && strlist.size() < 4 &&
                 strlist[0] != "ERROR")
        {
            VERBOSE(VB_IMPORTANT, loc_err +
                    QString("Did not get proper response from %1:%2")
                    .arg(host).arg(port));
            strlist.clear();
            strlist.push_back("ERROR");
            strlist.push_back("invalid response");
        }
    }

    if (strlist.empty() || strlist[0] == "ERROR")
    {
        lsock->DownRef();
        lsock = NULL;
        if (strlist.empty())
        {
            VERBOSE(VB_IMPORTANT, loc_err + "Failed to open socket, timeout");
        }
        else
        {
            VERBOSE(VB_IMPORTANT, loc_err + "Failed to open socket" +
                    ((strlist.size() >= 2) ?
                     QString(", error was %1").arg(strlist[1]) :
                     QString(", remote error")));
        }

        // Close the sockets if we received an error so that isOpen() will
        // return false if the caller tries to use the RemoteFile.
        Close();
    }

    return lsock;
}    

bool RemoteFile::Open(void)
{
    controlSock = openSocket(true);
    sock = openSocket(false);
    return isOpen();
}

void RemoteFile::Close(void)
{
    if (!controlSock)
        return;

    QStringList strlist( QString(query).arg(recordernum) );
    strlist << "DONE";

    lock.lock();
    controlSock->writeStringList(strlist);
    if (!controlSock->readStringList(strlist, true))
    {
        VERBOSE(VB_IMPORTANT, "Remote file timeout.");
    }
    
    if (sock)
    {
        sock->DownRef();
        sock = NULL;
    }    
    if (controlSock)
    {
        controlSock->DownRef();
        controlSock = NULL;
    } 

    lock.unlock();   
}

bool RemoteFile::DeleteFile(const QString &url)
{
    RemoteFile *rf = new RemoteFile();

    if (!rf)
        return false;

    rf->SetURL(url);
    bool ret = rf->DeleteFile();

    delete rf;

    return ret;
}

bool RemoteFile::DeleteFile(void)
{
    bool result      = false;
    QUrl qurl(path);
    QString filename = qurl.path();
    QString sgroup   = qurl.userName();

    if (!qurl.fragment().isEmpty() || path.right(1) == "#")
        filename = filename + "#" + qurl.fragment();

    if (filename.left(1) == "/")
        filename = filename.right(filename.length()-1);

    if (filename.isEmpty() || sgroup.isEmpty())
        return false;

    sock = openSocket(true);

    if (!sock)
        return false;

    QStringList strlist("DELETE_FILE");
    strlist << filename;
    strlist << sgroup;

    sock->writeStringList(strlist);
    if (!sock->readStringList(strlist, false))
    {
        VERBOSE(VB_IMPORTANT, "Remote file delete timeout.");
    }
    else if (strlist[0] == "1")
        result = true;

    sock->DownRef();
    sock = NULL;

    return result;
}

QString RemoteFile::GetFileHash(const QString &url)
{
    RemoteFile *rf = new RemoteFile();

    if (!rf)
        return false;

    rf->SetURL(url);
    QString ret = rf->GetFileHash();

    delete rf;

    return ret;
}


bool RemoteFile::Exists(const QString &url)
{
    RemoteFile *rf = new RemoteFile();

    if (!rf)
        return false;

    rf->SetURL(url);
    bool ret = rf->Exists();

    delete rf;

    return ret;
}

QString RemoteFile::GetFileHash(void)
{
    QString result;
    QUrl qurl(path);
    QString filename = qurl.path();
    QString sgroup   = qurl.userName();

    if (!qurl.fragment().isEmpty() || path.right(1) == "#")
        filename = filename + "#" + qurl.fragment();

    if (filename.left(1) == "/")
        filename = filename.right(filename.length()-1);

    if (filename.isEmpty() || sgroup.isEmpty())
        return false;

    sock = openSocket(true);

    if (!sock)
        return false;

    QStringList strlist("QUERY_FILE_HASH");
    strlist << filename;
    strlist << sgroup;

    sock->writeStringList(strlist);
    if (!sock->readStringList(strlist, false))
    {
        VERBOSE(VB_IMPORTANT, "Remote check file hash timeout.");
        result = QString();
    }
    else
        result = strlist[0];

    sock->DownRef();
    sock = NULL;

    return result;
}

bool RemoteFile::Exists(void)
{
    bool result      = false;
    QUrl qurl(path);
    QString filename = qurl.path();
    QString sgroup   = qurl.userName();

    if (!qurl.fragment().isEmpty() || path.right(1) == "#")
        filename = filename + "#" + qurl.fragment();

    if (filename.left(1) == "/")
        filename = filename.right(filename.length()-1);

    if (filename.isEmpty() || sgroup.isEmpty())
        return false;

    sock = openSocket(true);

    if (!sock)
        return false;

    QStringList strlist("QUERY_FILE_EXISTS");
    strlist << filename;
    strlist << sgroup;

    sock->writeStringList(strlist);
    if (!sock->readStringList(strlist, false))
    {
        VERBOSE(VB_IMPORTANT, "Remote check file exists timeout.");
    }
    else if (strlist[0] == "1")
        result = true;

    sock->DownRef();
    sock = NULL;

    return result;
}

void RemoteFile::Reset(void)
{
    if (!sock)
    {
        VERBOSE(VB_NETWORK, "RemoteFile::Reset(): Called with no socket");
        return;
    }

    while (sock->bytesAvailable() > 0)
    {
        int avail;
        char *trash;

        lock.lock();
        avail = sock->bytesAvailable();
        trash = new char[avail + 1];
        sock->readBlock(trash, avail);
        delete [] trash;
        lock.unlock();

        VERBOSE(VB_NETWORK, QString ("%1 bytes available during reset.")
                                      .arg(avail));
        usleep(30000);
    }
}

long long RemoteFile::Seek(long long pos, int whence, long long curpos)
{
    if (!sock)
    {
        VERBOSE(VB_NETWORK, "RemoteFile::Seek(): Called with no socket");
        return 0;
    }

    if (!sock->isOpen() || sock->error())
        return 0;

    if (!controlSock->isOpen() || controlSock->error())
        return 0;

    QStringList strlist( QString(query).arg(recordernum) );
    strlist << "SEEK";
    encodeLongLong(strlist, pos);
    strlist << QString::number(whence);
    if (curpos > 0)
        encodeLongLong(strlist, curpos);
    else
        encodeLongLong(strlist, readposition);

    lock.lock();
    controlSock->writeStringList(strlist);
    controlSock->readStringList(strlist);
    lock.unlock();

    long long retval = decodeLongLong(strlist, 0);
    readposition = retval;

    Reset();

    return retval;
}

int RemoteFile::Write(const void *data, int size)
{
    int recv = 0;
    int sent = 0;
    unsigned zerocnt = 0;
    bool error = false;
    bool response = false;

    if (!writemode)
    {
        VERBOSE(VB_NETWORK,
                "RemoteFile::Write(): Called when not in write mode");
        return -1;
    }

    if (!sock)
    {
        VERBOSE(VB_NETWORK, "RemoteFile::Write(): Called with no socket");
        return -1;
    }

    if (!sock->isOpen() || sock->error())
        return -1;
   
    if (!controlSock->isOpen() || controlSock->error())
        return -1;
    
    lock.lock();
    
    QStringList strlist( QString(query).arg(recordernum) );
    strlist << "WRITE_BLOCK";
    strlist << QString::number(size);
    controlSock->writeStringList(strlist);

    recv = size;
    while (sent < recv && !error && zerocnt++ < 50)
    {
        int ret = sock->writeBlock((char *)data + sent, recv - sent);
        if (ret > 0)
        {
            sent += ret;
        }
        else
        {
            VERBOSE(VB_IMPORTANT, "RemoteFile::Write(): socket error");
            error = true;
            break;
        }

        if (controlSock->bytesAvailable() > 0)
        {
            controlSock->readStringList(strlist, true);
            recv = strlist[0].toInt(); // -1 on backend error
            response = true;
        }
    }

    if (!error && !response)
    {
        if (controlSock->readStringList(strlist, true))
        {
            recv = strlist[0].toInt(); // -1 on backend error
        }
        else
        {
            VERBOSE(VB_IMPORTANT,
                    "RemoteFile::Write(): No response from control socket.");
            recv = -1;
        }
    }

    lock.unlock();

    VERBOSE(VB_NETWORK,
            QString("RemoteFile::Write(): reqd=%1, sent=%2, rept=%3, error=%4")
                    .arg(size).arg(sent).arg(recv).arg(error));

    if (recv < 0)
        return recv;

    if (error || recv != sent)
        sent = -1;

    return sent;
}

int RemoteFile::Read(void *data, int size)
{
    int recv = 0;
    int sent = 0;
    unsigned zerocnt = 0;
    bool error = false;
    bool response = false;
   
    if (!sock)
    {
        VERBOSE(VB_NETWORK, "RemoteFile::Read(): Called with no socket");
        return -1;
    }

    if (!sock->isOpen() || sock->error())
        return -1;
   
    if (!controlSock->isOpen() || controlSock->error())
        return -1;
    
    lock.lock();
    
    if (sock->bytesAvailable() > 0)
    {
        VERBOSE(VB_NETWORK, 
                "RemoteFile::Read(): Read socket not empty to start!");
        while (sock->waitForMore(5) > 0)
        {
            int avail = sock->bytesAvailable();
            char *trash = new char[avail + 1];
            sock->readBlock(trash, avail);
            delete [] trash;
        }
    }
    
    if (controlSock->bytesAvailable() > 0)
    {
        VERBOSE(VB_NETWORK, 
                "RemoteFile::Read(): Control socket not empty to start!");
        QStringList tempstrlist;
        controlSock->readStringList(tempstrlist);
    }
    
    QStringList strlist( QString(query).arg(recordernum) );
    strlist << "REQUEST_BLOCK";
    strlist << QString::number(size);
    controlSock->writeStringList(strlist);

    sent = size;
    
    while (recv < sent && !error && zerocnt++ < 50)
    {
        while (recv < sent && sock->waitForMore(200) > 0)
        {
            int ret = sock->readBlock(((char *)data) + recv, sent - recv);
            if (ret > 0)
            {
                recv += ret;
            }
            else if (sock->error() != MythSocket::NoError)
            {
                VERBOSE(VB_IMPORTANT, "RemoteFile::Read(): socket error");
                error = true;
                break;
            }
        }

        if (controlSock->bytesAvailable() > 0)
        {
            controlSock->readStringList(strlist, true);
            sent = strlist[0].toInt(); // -1 on backend error
            response = true;
        }
    } 
    
    if (!error && !response)
    {
        if (controlSock->readStringList(strlist, true))
        {
            sent = strlist[0].toInt(); // -1 on backend error
        }
        else
        {
            VERBOSE(VB_IMPORTANT, 
                   "RemoteFile::Read(): No response from control socket.");
            sent = -1;
        }
    }

    lock.unlock();

    VERBOSE(VB_NETWORK, QString("Read(): reqd=%1, rcvd=%2, rept=%3, error=%4")
                                .arg(size).arg(recv).arg(sent).arg(error));

    if (sent < 0)
        return sent;

    if (error || sent != recv)
        recv = -1;

    return recv;
}

bool RemoteFile::SaveAs(QByteArray &data)
{
    if (filesize < 0)
        return false;

    data.resize(filesize);
    Read(data.data(), filesize);

    return true;
}

void RemoteFile::SetTimeout(bool fast)
{
    if (timeoutisfast == fast)
        return;

    if (!sock)
    {
        VERBOSE(VB_NETWORK, "RemoteFile::SetTimeout(): Called with no socket");
        return;
    }

    if (!sock->isOpen() || sock->error())
        return;

    if (!controlSock->isOpen() || controlSock->error())
        return;

    QStringList strlist( QString(query).arg(recordernum) );
    strlist << "SET_TIMEOUT";
    strlist << QString::number((int)fast);

    lock.lock();
    controlSock->writeStringList(strlist);
    controlSock->readStringList(strlist);
    lock.unlock();

    timeoutisfast = fast;
}

/* vim: set expandtab tabstop=4 shiftwidth=4: */