/**************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Installer Framework.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
**************************************************************************/

#include "adminauthorization.h"

#include <QtCore/QFile>
#include <QDebug>

#include <QApplication>
#include <QInputDialog>
#include <QMessageBox>

#include <cstdlib>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef Q_OS_LINUX
#include <linux/limits.h>
#include <pty.h>
#else
#ifdef Q_OS_FREEBSD
#include <libutil.h>
#include <signal.h>
#else
#include <util.h>
#endif
#endif
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include <iostream>

#define SU_COMMAND "/usr/bin/sudo"
//#define SU_COMMAND "/bin/echo"

namespace QInstaller {

static QString getPassword(QWidget *parent)
{
    if (qobject_cast<QApplication*> (qApp) != 0) {
        bool ok = false;
        const QString result = QInputDialog::getText(parent, QObject::tr("Authorization required"),
           QObject::tr("Enter your password to authorize for sudo:"),
           QLineEdit::Password, QString(), &ok);
        return ok ? result : QString();
    } else {
        std::cout << QObject::tr("Authorization required").toStdString() << std::endl;
        std::cout << QObject::tr("Enter your password to authorize for sudo:").toStdString()
            << std::endl;
        std::string password;
        std::cin >> password;
        return QString::fromStdString(password);
    }
}

static void printError(QWidget *parent, const QString &value)
{
    if (qobject_cast<QApplication*> (qApp) != 0) {
        QMessageBox::critical(parent, QObject::tr( "Error acquiring admin rights" ), value,
            QMessageBox::Ok, QMessageBox::Ok);
    } else {
        std::cout << value.toStdString() << std::endl;
    }
}

bool AdminAuthorization::execute(QWidget *parent, const QString &program, const QStringList &arguments)
{
    const QString fallback = program + QLatin1String(" ") + arguments.join(QLatin1String(" "));
    qDebug() << "Fallback:" << fallback;

    // as we cannot pipe the password to su in QProcess, we need to setup a pseudo-terminal for it
    int masterFD = -1;
    int slaveFD = -1;
    char ptsn[ PATH_MAX ];

    if (::openpty(&masterFD, &slaveFD, ptsn, 0, 0))
        return false;

    masterFD = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (masterFD < 0)
        return false;

    const QByteArray ttyName = ::ptsname(masterFD);

    if (::grantpt(masterFD)) {
        ::close(masterFD);
        return false;
    }

    ::revoke(ttyName);
    ::unlockpt(masterFD);

    slaveFD = ::open(ttyName, O_RDWR | O_NOCTTY);
    if (slaveFD < 0) {
        ::close(masterFD);
        return false;
    }

    ::fcntl(masterFD, F_SETFD, FD_CLOEXEC);
    ::fcntl(slaveFD, F_SETFD, FD_CLOEXEC);
    int pipedData[2];
    if (pipe(pipedData) != 0)
        return false;

    int flags = ::fcntl(pipedData[0], F_GETFL);
    if (flags != -1)
        ::fcntl(pipedData[0], F_SETFL, flags | O_NONBLOCK);

    flags = ::fcntl(masterFD, F_GETFL);
    if (flags != -1)
        ::fcntl(masterFD, F_SETFL, flags | O_NONBLOCK);

    pid_t child = fork();

    if (child < -1) {
        ::close(masterFD);
        ::close(slaveFD);
        ::close(pipedData[0]);
        ::close(pipedData[1]);
        return false;
    }

    // parent process
    else if (child > 0) {
        ::close(slaveFD);
        //close writing end of pipe
        ::close(pipedData[1]);

        QRegExp re(QLatin1String("[Pp]assword.*:"));
        QByteArray data;
        QByteArray errData;
        int bytes = 0;
        int errBytes = 0;
        char buf[1024];
        char errBuf[1024];
        int status;
        bool statusValid = false;
        while (bytes >= 0) {
            const pid_t waitResult = ::waitpid(child, &status, WNOHANG);
            if (waitResult == -1) {
                break;
            }
            if (waitResult == child) {
                statusValid = true;
                break;
            }
            bytes = ::read(masterFD, buf, 1023);
            if (bytes == -1 && errno == EAGAIN)
                bytes = 0;
            else if (bytes > 0)
                data.append(buf, bytes);
            errBytes = ::read(pipedData[0], errBuf, 1023);
            if (errBytes > 0)
            {
                errData.append(errBuf, errBytes);
                errBytes=0;
            }
            if (bytes > 0) {
                const QString line = QString::fromLatin1(data);
                if (re.indexIn(line) != -1) {
                    const QString password = getPassword(parent);
                    if (password.isEmpty()) {
                        QByteArray pwd = password.toLatin1();
                        for (int i = 0; i < 3; ++i) {
                            ::write(masterFD, pwd.data(), pwd.length());
                            ::write(masterFD, "\n", 1);
                        }
                        return false;
                    }
                    QByteArray pwd = password.toLatin1();
                    ::write(masterFD, pwd.data(), pwd.length());
                    ::write(masterFD, "\n", 1);
                    ::read(masterFD, buf, pwd.length() + 1);
                }
            }
            if (bytes == 0)
                ::usleep(100000);
        }

        while (true) {
            errBytes = ::read(pipedData[0], errBuf, 1023);
            if (errBytes == -1 && errno == EAGAIN) {
                ::usleep(100000);
                continue;
            }

            if (errBytes <= 0)
                break;

            errData.append(errBuf, errBytes);
        }

        const bool success = statusValid && WIFEXITED(status) && WEXITSTATUS(status) == 0;

        if (!success && !errData.isEmpty()) {
            printError(parent, QString::fromLocal8Bit(errData.constData()));
        }

        ::close(pipedData[0]);
        return success;
    }

    // child process
    else {
        ::close(pipedData[0]);
        // Reset signal handlers
        for (int sig = 1; sig < NSIG; ++sig)
            signal(sig, SIG_DFL);
        signal(SIGHUP, SIG_IGN);

        ::setsid();

        ::ioctl(slaveFD, TIOCSCTTY, 1);
        int pgrp = ::getpid();
        ::tcsetpgrp(slaveFD, pgrp);

        ::dup2(slaveFD, 0);
        ::dup2(slaveFD, 1);
        ::dup2(pipedData[1], 2);

        // close all file descriptors
        struct rlimit rlp;
        getrlimit(RLIMIT_NOFILE, &rlp);
        for (int i = 3; i < static_cast<int>(rlp.rlim_cur); ++i)
            ::close(i);

        char **argp = (char **) ::malloc((arguments.count() + 4) * sizeof(char *));
        QList<QByteArray> args;
        args.push_back(SU_COMMAND);
        args.push_back("-b");
        args.push_back(program.toLocal8Bit());
        for (QStringList::const_iterator it = arguments.begin(); it != arguments.end(); ++it)
            args.push_back(it->toLocal8Bit());

        int i = 0;
        for (QList<QByteArray>::iterator it = args.begin(); it != args.end(); ++it, ++i)
            argp[i] = it->data();
        argp[i] = 0;

        ::unsetenv("LANG");
        ::unsetenv("LC_ALL");

        int exitStatus = 0;
        if (::execv(SU_COMMAND, argp) == -1)
            exitStatus = -errno;
        _exit(exitStatus);
        return false;
    }
}

// has no guarantee to work
bool AdminAuthorization::hasAdminRights()
{
    return getuid() == 0;
}

} // namespace QInstaller
