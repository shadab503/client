/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QSslConfiguration>
#include <QBuffer>
#include <QXmlStreamReader>
#include <QStringList>
#include <QStack>

#include <QDebug>

#include "json.h"

#include "mirall/networkjobs.h"
#include "mirall/account.h"

namespace Mirall {

AbstractNetworkJob::AbstractNetworkJob(Account *account, const QString &path, QObject *parent)
    : QObject(parent)
    , _reply(0)
    , _account(account)
    , _path(path)
{
}

void AbstractNetworkJob::setReply(QNetworkReply *reply)
{
    _reply = reply;
}

QNetworkReply *AbstractNetworkJob::takeReply()
{
    QNetworkReply *reply = _reply;
    _reply = 0;
    return reply;
}

void AbstractNetworkJob::setAccount(Account *account)
{
    _account = account;
}

void AbstractNetworkJob::setPath(const QString &path)
{
    _path = path;
}

void AbstractNetworkJob::slotError()
{
    qDebug() << metaObject()->className() << "Error:" << _reply->errorString();
    emit networkError(_reply->error(), _reply->errorString());
    deleteLater();
}

void AbstractNetworkJob::setupConnections(QNetworkReply *reply)
{
    connect( reply, SIGNAL( finished()), SLOT(slotFinished()) );
    connect( reply, SIGNAL(error(QNetworkReply::NetworkError)),
             this, SLOT(slotError()));

//    connect( reply, SIGNAL(error(QNetworkReply::NetworkError)),
//             ownCloudInfo::instance(), SLOT(slotError(QNetworkReply::NetworkError)));
}

QNetworkReply* AbstractNetworkJob::davRequest(const QByteArray &verb, const QString &relPath,
                                              QNetworkRequest req, QIODevice *data)
{
    return _account->davRequest(verb, relPath, req, data);
}

QNetworkReply* AbstractNetworkJob::getRequest(const QString &relPath)
{
    return _account->getRequest(relPath);
}

AbstractNetworkJob::~AbstractNetworkJob() {
    _reply->deleteLater();
}

/*********************************************************************************************/

RequestEtagJob::RequestEtagJob(Account *account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
    QNetworkRequest req;
    if (path.isEmpty() || path == QLatin1String("/")) {
        /* For the root directory, we need to query the etags of all the sub directories
         * because, at the time I am writing this comment (Owncloud 5.0.9), the etag of the
         * root directory is not updated when the sub directories changes */
        req.setRawHeader("Depth", "1");
    } else {
        req.setRawHeader("Depth", "0");
    }
    QByteArray xml("<?xml version=\"1.0\" ?>\n"
                   "<d:propfind xmlns:d=\"DAV:\">\n"
                   "  <d:prop>\n"
                   "    <d:getetag/>\n"
                   "  </d:prop>\n"
                   "</d:propfind>\n");
    QBuffer *buf = new QBuffer;
    buf->setData(xml);
    buf->open(QIODevice::ReadOnly);
    // assumes ownership
    setReply(davRequest("PROPFIND", path, req, buf));
    buf->setParent(reply());
    setupConnections(reply());

    if( reply()->error() != QNetworkReply::NoError ) {
        qDebug() << "getting etag: request network error: " << reply()->errorString();
    }

}

void RequestEtagJob::slotFinished()
{
    if (reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute) == 207) {
        // Parse DAV response
        QXmlStreamReader reader(reply());
        reader.addExtraNamespaceDeclaration(QXmlStreamNamespaceDeclaration("d", "DAV:"));
        QString etag;
        while (!reader.atEnd()) {
            QXmlStreamReader::TokenType type = reader.readNext();
            if (type == QXmlStreamReader::StartElement &&
                    reader.namespaceUri() == QLatin1String("DAV:")) {
                QString name = reader.name().toString();
                if (name == QLatin1String("getetag")) {
                    etag += reader.readElementText();
                }
            }
        }
        emit etagRetreived(etag);
    }
    deleteLater();
}

/*********************************************************************************************/

MkColJob::MkColJob(Account *account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
     // assumes ownership
    QNetworkReply *reply = davRequest("MKCOL", path);
    setReply(reply);
    setupConnections(reply);
}

void MkColJob::slotFinished()
{
    // ### useful error handling?
    // QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    emit finished();
    deleteLater();
}

/*********************************************************************************************/

LsColJob::LsColJob(Account *account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
    QNetworkRequest req;
    req.setRawHeader("Depth", "1");
    QByteArray xml("<?xml version=\"1.0\" ?>\n"
                   "<d:propfind xmlns:d=\"DAV:\">\n"
                   "  <d:prop>\n"
                   "    <d:resourcetype/>\n"
                   "  </d:prop>\n"
                   "</d:propfind>\n");
    QBuffer *buf = new QBuffer;
    buf->setData(xml);
    buf->open(QIODevice::ReadOnly);
    QNetworkReply *reply = davRequest("PROPFIND", path, req, buf);
    buf->setParent(reply);
    setReply(reply);
    setupConnections(reply);
}

void LsColJob::slotFinished()
{
    if (reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute) == 207) {
        // Parse DAV response
        QXmlStreamReader reader(reply());
        reader.addExtraNamespaceDeclaration(QXmlStreamNamespaceDeclaration("d", "DAV:"));

        QStringList folders;
        QString currentItem;

        while (!reader.atEnd()) {
            QXmlStreamReader::TokenType type = reader.readNext();
            if (type == QXmlStreamReader::StartElement &&
                    reader.namespaceUri() == QLatin1String("DAV:")) {
                QString name = reader.name().toString();
                if (name == QLatin1String("href")) {
                    currentItem = reader.readElementText();
                } else if (name == QLatin1String("collection") &&
                           !currentItem.isEmpty()) {
                    folders.append(QUrl::fromEncoded(currentItem.toLatin1()).path());
                    currentItem.clear();
                }
            }
        }
        emit directoryListing(folders);
    }

    deleteLater();
}

/*********************************************************************************************/

CheckServerJob::CheckServerJob(Account *account, bool followRedirect, QObject *parent)
    : AbstractNetworkJob(account, QLatin1String("/status.php") , parent)
    , _followRedirects(followRedirect)
    , _redirectCount(0)
{
    // ### perform update of certificate chain
    setReply(getRequest(path()));
    setupConnections(reply());
}

QString CheckServerJob::version(const QVariantMap &info)
{
    return info.value(QLatin1String("version")).toString();
}

QString CheckServerJob::versionString(const QVariantMap &info)
{
    return info.value(QLatin1String("versionstring")).toString();
}

bool CheckServerJob::installed(const QVariantMap &info)
{
    return info.value(QLatin1String("installed")).toBool();
}

void CheckServerJob::slotFinished()
{
    // ### this should no longer be needed
    if( reply()->error() == QNetworkReply::NoError && reply()->size() == 0 ) {
        // This seems to be a bit strange behaviour of QNetworkAccessManager.
        // It calls the finised slot multiple times but only the first read wins.
        // That happend when the code connected the finished signal of the manager.
        // It did not happen when the code connected to the reply finish signal.
        qDebug() << "WRN: NetworkReply with not content but also no error! " << reply();
        deleteLater();
        return;
    }

    account()->setCertificateChain(reply()->sslConfiguration().peerCertificateChain());

    // ### the qDebugs here should be exported via displayErrors() so they
    // ### can be presented to the user if the job executor has a GUI
    QUrl requestedUrl = reply()->request().url();
    QUrl redirectUrl = reply()->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (!redirectUrl.isEmpty()) {
        _redirectCount++;
        if (requestedUrl.scheme() == QLatin1String("https") &&
                redirectUrl.scheme() == QLatin1String("http")) {
                qDebug() << Q_FUNC_INFO << "HTTPS->HTTP downgrade detected!";
        } else if (requestedUrl == redirectUrl || _redirectCount >= MAX_REDIRECTS) {
                qDebug() << Q_FUNC_INFO << "Redirect loop detected!";
        } else {
            takeReply()->deleteLater();
            // ### FIXME
            //setReply(getRequest(redirectUrl));
            setupConnections(reply());
            return;
        }
    }

    bool success = false;
    QVariantMap status = QtJson::parse(QString::fromUtf8(reply()->readAll()), success).toMap();
    // empty or invalid response
    if (!success || status.isEmpty()) {
        qDebug() << "status.php from server is not valid JSON!";
    }

    qDebug() << "status.php returns: " << status << " " << reply()->error() << " Reply: " << reply();
    if( status.contains("installed")
            && status.contains("version")
            && status.contains("versionstring") ) {
        emit instanceFound(status);
    } else {
        qDebug() << "No proper answer on " << requestedUrl;
    }
    deleteLater();
}

PropfindJob::PropfindJob(Account *account, const QString &path,
                         QList<QByteArray> properties,
                         QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
    if (properties.isEmpty()) {
        properties << "allprop";
    }
    QNetworkRequest req;
    req.setRawHeader("Depth", "0");
    QByteArray propStr;
    foreach (const QByteArray &prop, properties) {
        propStr += "    <d:" + prop + " />\n";
    }
    QByteArray xml = "<?xml version=\"1.0\" ?>\n"
                     "<d:propfind xmlns:d=\"DAV:\">\n"
                     "  <d:prop>\n"
                     + propStr +
                     "  </d:prop>\n"
                     "</d:propfind>\n";

    QBuffer *buf = new QBuffer;
    buf->setData(xml);
    buf->open(QIODevice::ReadOnly);
    setReply(davRequest("PROPFIND", path, req, buf));
    buf->setParent(reply());
    setupConnections(reply());
}

void PropfindJob::slotFinished()
{
    int http_result_code = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (http_result_code == 207) {
        // Parse DAV response
        QXmlStreamReader reader(reply());
        reader.addExtraNamespaceDeclaration(QXmlStreamNamespaceDeclaration("d", "DAV:"));

        QVariantMap items;
        // introduced to nesting is ignored
        QStack<QString> curElement;

        while (!reader.atEnd()) {
            QXmlStreamReader::TokenType type = reader.readNext();
            if (type == QXmlStreamReader::StartElement &&
                    reader.namespaceUri() == QLatin1String("DAV:")) {
                if (curElement.isEmpty()) {
                    curElement.push(reader.name().toString());
                    items.insert(reader.name().toString(), reader.text().toString());
                }
            }
            if (type == QXmlStreamReader::EndElement &&
                    reader.namespaceUri() == QLatin1String("DAV:")) {
                if(curElement.top() == reader.name()) {
                    curElement.pop();
                }
            }

        }
        emit result(items);
    } else {
        qDebug() << "Quota request *not* successful, http result code is " << http_result_code;
    }

    deleteLater();
}

} // namespace Mirall
