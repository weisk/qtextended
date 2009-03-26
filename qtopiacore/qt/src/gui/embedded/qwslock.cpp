/****************************************************************************
**
** Copyright (C) 2008 Nokia Corporation and/or its subsidiary(-ies).
** Contact: Qt Software Information (qt-info@nokia.com)
**
** This file is part of the QtGui module of the Qt Toolkit.
**
** Commercial Usage
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License versions 2.0 or 3.0 as published by the Free
** Software Foundation and appearing in the file LICENSE.GPL included in
** the packaging of this file.  Please review the following information
** to ensure GNU General Public Licensing requirements will be met:
** http://www.fsf.org/licensing/licenses/info/GPLv2.html and
** http://www.gnu.org/copyleft/gpl.html.  In addition, as a special
** exception, Nokia gives you certain additional rights. These rights
** are described in the Nokia Qt GPL Exception version 1.3, included in
** the file GPL_EXCEPTION.txt in this package.
**
** Qt for Windows(R) Licensees
** As a special exception, Nokia, as the sole copyright holder for Qt
** Designer, grants users of the Qt/Eclipse Integration plug-in the
** right for the Qt/Eclipse Integration to link to functionality
** provided by Qt Designer and its related libraries.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at qt-sales@nokia.com.
**
****************************************************************************/

#include "qwslock_p.h"

#ifndef QT_NO_QWS_MULTIPROCESS

#include "qwssignalhandler_p.h"

#include <qglobal.h>
#include <qdebug.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/time.h>
#include <time.h>
#ifdef Q_OS_LINUX
#include <linux/version.h>
#endif
#include <unistd.h>

QT_BEGIN_NAMESPACE

#ifdef QT_NO_SEMAPHORE
#error QWSLock currently requires semaphores
#endif

#ifndef Q_OS_BSD4
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo  *__buf;
};
#endif

QWSLock::QWSLock()
{
    semId = semget(IPC_PRIVATE, 3, IPC_CREAT | 0666);

    if (semId == -1) {
        perror("QWSLock::QWSLock");
        qFatal("Unable to create semaphore");
    }
    QWSSignalHandler::instance()->addSemaphore(semId);

    semun semval;
    semval.val = 1;

    if (semctl(semId, BackingStore, SETVAL, semval) == -1) {
        perror("QWSLock::QWSLock");
        qFatal("Unable to initialize backingstore semaphore");
    }
    lockCount[BackingStore] = 0;

    if (semctl(semId, Communication, SETVAL, semval) == -1) {
        perror("QWSLock::QWSLock");
        qFatal("Unable to initialize communication semaphore");
    }
    lockCount[Communication] = 0;

    semval.val = 0;
    if (semctl(semId, RegionEvent, SETVAL, semval) == -1) {
        perror("QWSLock::QWSLock");
        qFatal("Unable to initialize region event semaphore");
    }
}

QWSLock::QWSLock(int id)
{
    semId = id;
    QWSSignalHandler::instance()->addSemaphore(semId);
    lockCount[0] = lockCount[1] = 0;
}

QWSLock::~QWSLock()
{
    if (semId == -1)
        return;
    QWSSignalHandler::instance()->removeSemaphore(semId);
}

static bool forceLock(int semId, int semNum, int)
{
    int ret;
    do {
        sembuf sops = { semNum, -1, 0 };

        // As the BackingStore lock is a mutex, and only one process may own
        // the lock, it's safe to use SEM_UNDO. On the other hand, the
        // Communication lock is locked by the client but unlocked by the
        // server and therefore can't use SEM_UNDO.
        if (semNum == QWSLock::BackingStore)
            sops.sem_flg |= SEM_UNDO;

        ret = semop(semId, &sops, 1);
        if (ret == -1 && errno != EINTR)
            qDebug("QWSLock::lock: %s", strerror(errno));
    } while (ret == -1 && errno == EINTR);

    return (ret != -1);
}

static bool up(int semId, int semNum)
{
    int ret;
    do {
        sembuf sops = { semNum, 1, 0 };
        ret = semop(semId, &sops, 1);
        if (ret == -1 && errno != EINTR)
            qDebug("QWSLock::up: %s", strerror(errno));
    } while (ret == -1 && errno == EINTR);

    return (ret != -1);
}

static bool down(int semId, int semNum)
{
    int ret;
    do {
        sembuf sops = { semNum, -1, 0 };
        ret = semop(semId, &sops, 1);
        if (ret == -1 && errno != EINTR)
            qDebug("QWSLock::down: %s", strerror(errno));
    } while (ret == -1 && errno == EINTR);

    return (ret != -1);
}

static int getValue(int semId, int semNum)
{
    int ret;
    do {
        ret = semctl(semId, semNum, GETVAL, 0);
        if (ret == -1 && errno != EINTR)
            qDebug("QWSLock::getValue: %s", strerror(errno));
    } while (ret == -1 && errno == EINTR);

    return ret;
}

bool QWSLock::lock(LockType type, int timeout)
{
    if (type == RegionEvent)
        return up(semId, RegionEvent);

    if (hasLock(type)) {
        ++lockCount[type];
        return true;
    }

    if (!forceLock(semId, type, timeout))
        return false;
    ++lockCount[type];
    return true;
}

bool QWSLock::hasLock(LockType type)
{
    if (type == RegionEvent)
        return (getValue(semId, RegionEvent) == 0);

    return (lockCount[type] > 0);
}

void QWSLock::unlock(LockType type)
{
    if (type == RegionEvent) {
        down(semId, RegionEvent);
        return;
    }

    if (hasLock(type)) {
        --lockCount[type];
        if (hasLock(type))
            return;
    }

    const int semNum = type;
    int ret;
    do {
        sembuf sops = {semNum, 1, 0};
        if (semNum == QWSLock::BackingStore)
            sops.sem_flg |= SEM_UNDO;

        ret = semop(semId, &sops, 1);
        if (ret == -1 && errno != EINTR)
            qDebug("QWSLock::unlock: %s", strerror(errno));
    } while (ret == -1 && errno == EINTR);
}

bool QWSLock::wait(LockType type, int timeout)
{
    bool ok = forceLock(semId, type, timeout);
    if (ok)
        unlock(type);
    return ok;
}

QT_END_NAMESPACE

#endif // QT_NO_QWS_MULTIPROCESS