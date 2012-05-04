/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 *
 *   Copyright 2010-2011, Leo Franchi <lfranchi@kde.org>
 *
 *   Tomahawk is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#include "AtticaManager.h"

#include "utils/TomahawkUtils.h"
#include "TomahawkSettingsGui.h"
#include "Pipeline.h"

#include <attica/downloaditem.h>
#include <quazip.h>
#include <quazipfile.h>

#include <QNetworkReply>
#include <QTemporaryFile>
#include <QDir>
#include <QTimer>

#include "utils/Logger.h"
#include "accounts/ResolverAccount.h"
#include "accounts/AccountManager.h"

using namespace Attica;

AtticaManager* AtticaManager::s_instance = 0;


AtticaManager::AtticaManager( QObject* parent )
    : QObject( parent )
    , m_resolverJobsLoaded( 0 )
{
    connect( &m_manager, SIGNAL( providerAdded( Attica::Provider ) ), this, SLOT( providerAdded( Attica::Provider ) ) );

    // resolvers
//    m_manager.addProviderFile( QUrl( "http://bakery.tomahawk-player.org/resolvers/providers.xml" ) );
    m_manager.addProviderFile( QUrl( "http://localhost/resolvers/providers.xml" ) );

    qRegisterMetaType< Attica::Content >( "Attica::Content" );
}


AtticaManager::~AtticaManager()
{
    savePixmapsToCache();
}


void
AtticaManager::loadPixmapsFromCache()
{
    QDir cacheDir = TomahawkUtils::appDataDir();
    if ( !cacheDir.cd( "atticacache" ) ) // doesn't exist, no cache
        return;

    qDebug() << "Loading resolvers from cache dir:" << cacheDir.absolutePath();
    qDebug() << "Currently we know about these resolvers:" << m_resolverStates.keys();
    foreach ( const QString& file, cacheDir.entryList( QStringList() << "*.png", QDir::Files | QDir::NoSymLinks ) )
    {
        // load all the pixmaps
        QFileInfo info( file );
        if ( !m_resolverStates.contains( info.baseName() ) )
        {
            tLog() << "Found resolver icon cached for resolver we no longer see in synchrotron repo:" << info.baseName();
            continue;
        }

        QPixmap* icon = new QPixmap( cacheDir.absoluteFilePath( file ) );
        if ( !icon->isNull() )
        {
            m_resolverStates[ info.baseName() ].pixmap = icon;
        }
    }
}


void
AtticaManager::savePixmapsToCache()
{
    QDir cacheDir = TomahawkUtils::appDataDir();
    if ( !cacheDir.cd( "atticacache" ) ) // doesn't exist, create
    {
        cacheDir.mkdir( "atticacache" );
        cacheDir.cd( "atticache" );
    }

    foreach( const QString& id, m_resolverStates.keys() )
    {
        if ( !m_resolverStates[ id ].pixmap || !m_resolverStates[ id ].pixmapDirty )
            continue;

        const QString filename = cacheDir.absoluteFilePath( QString( "%1.png" ).arg( id ) );
        QFile f( filename );
        if ( !f.open( QIODevice::WriteOnly ) )
        {
            tLog() << "Failed to open cache file for writing:" << filename;
        }
        else
        {
            if ( !m_resolverStates[ id ].pixmap->save( &f ) )
            {
                tLog() << "Failed to save pixmap into opened file for writing:" << filename;
            }
        }
    }
}


QPixmap
AtticaManager::iconForResolver( const Content& resolver )
{
    if ( !m_resolverStates[ resolver.id() ].pixmap )
        return QPixmap();

    return *m_resolverStates.value( resolver.id() ).pixmap;
}


Content::List
AtticaManager::resolvers() const
{
    return m_resolvers;
}


Content
AtticaManager::resolverForId( const QString& id ) const
{
    foreach ( const Attica::Content& c, m_resolvers )
    {
        if ( c.id() == id )
            return c;
    }

    return Content();
}



AtticaManager::ResolverState
AtticaManager::resolverState ( const Content& resolver ) const
{
    if ( !m_resolverStates.contains( resolver.id() ) )
    {
        return AtticaManager::Uninstalled;
    }

    return m_resolverStates[ resolver.id() ].state;
}


bool
AtticaManager::resolversLoaded() const
{
    return !m_resolvers.isEmpty();
}


QString
AtticaManager::pathFromId( const QString& resolverId ) const
{
    if ( !m_resolverStates.contains( resolverId ) )
        return QString();

    return m_resolverStates.value( resolverId ).scriptPath;
}


void
AtticaManager::uploadRating( const Content& c )
{
    m_resolverStates[ c.id() ].userRating = c.rating();

    for ( int i = 0; i < m_resolvers.count(); i++ )
    {
        if ( m_resolvers[ i ].id() == c.id() )
        {
            Attica::Content atticaContent = m_resolvers[ i ];
            atticaContent.setRating( c.rating() );
            m_resolvers[ i ] = atticaContent;
            break;
        }
    }

    TomahawkSettingsGui::instanceGui()->setAtticaResolverStates( m_resolverStates );

    PostJob* job = m_resolverProvider.voteForContent( c.id(), (uint)c.rating() );
    connect( job, SIGNAL( finished( Attica::BaseJob* ) ), job, SLOT( deleteLater() ) );

    job->start();

    emit resolverStateChanged( c.id() );
}


bool
AtticaManager::userHasRated( const Content& c ) const
{
    return m_resolverStates[ c.id() ].userRating != -1;
}


bool
AtticaManager::hasCustomAccountForAttica( const QString &id ) const
{
    // Only last.fm at the moment contains a custom account
    if ( id == "lastfm" )
        return true;

    return false;
}


Tomahawk::Accounts::Account*
AtticaManager::customAccountForAttica( const QString &id ) const
{
    return m_customAccounts.value( id );
}


void
AtticaManager::registerCustomAccount( const QString &atticaId, Tomahawk::Accounts::Account *account )
{
    m_customAccounts.insert( atticaId, account );
}


AtticaManager::Resolver
AtticaManager::resolverData(const QString &atticaId) const
{
    return m_resolverStates.value( atticaId );
}


void
AtticaManager::providerAdded( const Provider& provider )
{
    if ( provider.name() == "Tomahawk Resolvers" )
    {
        m_resolverProvider = provider;
        m_resolvers.clear();

        m_resolverStates = TomahawkSettingsGui::instanceGui()->atticaResolverStates();

        ListJob<Category>* job = m_resolverProvider.requestCategories();
        connect( job, SIGNAL( finished( Attica::BaseJob* ) ), this, SLOT( categoriesReturned( Attica::BaseJob* ) ) );
        job->start();
    }
}


void
AtticaManager::categoriesReturned( BaseJob* j )
{
    ListJob< Category >* job = static_cast< ListJob< Category >* >( j );

    Category::List categories = job->itemList();
    foreach ( const Category& category, categories )
    {
        ListJob< Content >* job = m_resolverProvider.searchContents( Category::List() << category, QString(), Provider::Downloads, 0, 50 );

        if ( category.name() == "Resolver" )
            connect( job, SIGNAL( finished( Attica::BaseJob* ) ), this, SLOT( resolversList( Attica::BaseJob* ) ) );
        else if ( category.name() == "BinaryResolver" )
            connect( job, SIGNAL( finished( Attica::BaseJob* ) ), this, SLOT( binaryResolversList( Attica::BaseJob* ) ) );

        job->start();
    }
}


void
AtticaManager::resolversList( BaseJob* j )
{
    ListJob< Content >* job = static_cast< ListJob< Content >* >( j );

    m_resolvers.append( job->itemList() );

    // Sanity check. if any resolvers are installed that don't exist on the hd, remove them.
    foreach ( const QString& rId, m_resolverStates.keys() )
    {
        if ( m_resolverStates[ rId ].state == Installed ||
             m_resolverStates[ rId ].state == NeedsUpgrade )
        {
            if ( m_resolverStates[ rId ].binary )
                continue;

            // Guess location on disk
            QDir dir( QString( "%1/atticaresolvers/%2" ).arg( TomahawkUtils::appDataDir().absolutePath() ).arg( rId ) );
            if ( !dir.exists() )
            {
                // Uh oh
                qWarning() << "Found attica resolver marked as installed that didn't exist on disk! Setting to uninstalled: " << rId << dir.absolutePath();
                m_resolverStates[ rId ].state = Uninstalled;
                TomahawkSettingsGui::instanceGui()->setAtticaResolverState( rId, Uninstalled );
            }
        }
    }

    // load icon cache from disk, and fetch any we are missing
    loadPixmapsFromCache();

    foreach ( Content resolver, m_resolvers )
    {
        if ( !m_resolverStates.contains( resolver.id() ) )
            m_resolverStates.insert( resolver.id(), Resolver() );

        if ( !m_resolverStates.value( resolver.id() ).pixmap && !resolver.icons().isEmpty() && !resolver.icons().first().url().isEmpty() )
        {
            QNetworkReply* fetch = TomahawkUtils::nam()->get( QNetworkRequest( resolver.icons().first().url() ) );
            fetch->setProperty( "resolverId", resolver.id() );

            connect( fetch, SIGNAL( finished() ), this, SLOT( resolverIconFetched() ) );
        }
    }

    syncServerData();

    if ( ++m_resolverJobsLoaded == 2 )
        emit resolversLoaded( m_resolvers );
}


void
AtticaManager::binaryResolversList( BaseJob* j )
{
    ListJob< Content >* job = static_cast< ListJob< Content >* >( j );

    Content::List binaryResolvers = job->itemList();

    // NOTE: No binary support for linux distros
    QString platform;
#ifdef Q_OS_MAC
    platform = "osx";
#elif Q_OS_WIN
    platform = "win";
#endif

    // NOTE HACK
    // At the moment we are going to assume that all binary resolvers also have an associated full-fledged Tomahawk Account
    //  like SpotifyAccount.

    foreach ( const Content& c, binaryResolvers )
    {
        if ( !c.attribute( "typeid" ).isEmpty() && c.attribute( "typeid" ) == platform )
        {
            // We have a binary resolver for this platform
            m_resolvers.append( c );
            if ( !m_resolverStates.contains( c.id() ) )
            {
                Resolver r;
                r.binary = true;
                m_resolverStates.insert( c.id(), r );
            }


        }
    }

    if ( ++m_resolverJobsLoaded == 2 )
        emit resolversLoaded( m_resolvers );
}


void
AtticaManager::resolverIconFetched()
{
    QNetworkReply* reply = qobject_cast< QNetworkReply* >( sender() );
    Q_ASSERT( reply );

    const QString resolverId = reply->property( "resolverId" ).toString();

    if ( !reply->error() == QNetworkReply::NoError )
    {
        tLog() << "Failed to fetch resolver icon image:" << reply->errorString();
        return;
    }

    QByteArray data = reply->readAll();
    QPixmap* icon = new QPixmap;
    icon->loadFromData( data );
    m_resolverStates[ resolverId ].pixmap = icon;
    m_resolverStates[ resolverId ].pixmapDirty = true;
}


void
AtticaManager::syncServerData()
{
    // look for any newer. m_resolvers has list from server, and m_resolverStates will contain any locally installed ones
    // also update ratings
    foreach ( const QString& id, m_resolverStates.keys() )
    {
        Resolver r = m_resolverStates[ id ];
        for ( int i = 0; i < m_resolvers.size(); i++ )
        {
            Attica::Content upstream = m_resolvers[ i ];
            // same resolver
            if ( id != upstream.id() )
                continue;

            // Update our rating with the server's idea of rating if we haven't rated it
            if ( m_resolverStates[ id ].userRating != -1 )
            {
                upstream.setRating( m_resolverStates[ id ].userRating );
                m_resolvers[ i ] = upstream;
            }

            // DO we need to upgrade?
            if ( ( r.state == Installed || r.state == NeedsUpgrade ) &&
                 !upstream.version().isEmpty() )
            {
                if ( TomahawkUtils::newerVersion( r.version, upstream.version() ) )
                {
                    m_resolverStates[ id ].state = NeedsUpgrade;
                    QMetaObject::invokeMethod( this, "upgradeResolver", Qt::QueuedConnection, Q_ARG( Attica::Content, upstream ) );
                }
            }
        }
    }
}


void
AtticaManager::installResolver( const Content& resolver, bool autoCreateAccount )
{
    Q_ASSERT( !resolver.id().isNull() );

    if ( m_resolverStates[ resolver.id() ].state != Upgrading )
        m_resolverStates[ resolver.id() ].state = Installing;

    m_resolverStates[ resolver.id() ].scriptPath = resolver.attribute( "mainscript" );
    m_resolverStates[ resolver.id() ].version = resolver.version();
    emit resolverStateChanged( resolver.id() );

    ItemJob< DownloadItem >* job = m_resolverProvider.downloadLink( resolver.id() );
    connect( job, SIGNAL( finished( Attica::BaseJob* ) ), this, SLOT( resolverDownloadFinished( Attica::BaseJob* ) ) );
    job->setProperty( "resolverId", resolver.id() );
    job->setProperty( "createAccount", autoCreateAccount );

    job->start();
}


void
AtticaManager::upgradeResolver( const Content& resolver )
{
    Q_ASSERT( m_resolverStates.contains( resolver.id() ) );
    Q_ASSERT( m_resolverStates[ resolver.id() ].state == NeedsUpgrade );

    if ( !m_resolverStates.contains( resolver.id() ) || m_resolverStates[ resolver.id() ].state != NeedsUpgrade )
        return;

    m_resolverStates[ resolver.id() ].state = Upgrading;
    emit resolverStateChanged( resolver.id() );

    uninstallResolver( resolver );
    installResolver( resolver, false );
}


void
AtticaManager::resolverDownloadFinished ( BaseJob* j )
{
    ItemJob< DownloadItem >* job = static_cast< ItemJob< DownloadItem >* >( j );

    if ( job->metadata().error() == Attica::Metadata::NoError )
    {
        DownloadItem item = job->result();
        QUrl url = item.url();
        // download the resolver itself :)
        QNetworkReply* reply = TomahawkUtils::nam()->get( QNetworkRequest( url ) );
        connect( reply, SIGNAL( finished() ), this, SLOT( payloadFetched() ) );
        reply->setProperty( "resolverId", job->property( "resolverId" ) );
        reply->setProperty( "createAccount", job->property( "createAccount" ) );
    }
    else
    {
        tLog() << "Failed to do resolver download job!" << job->metadata().error();
    }
}


void
AtticaManager::payloadFetched()
{
    QNetworkReply* reply = qobject_cast< QNetworkReply* >( sender() );
    Q_ASSERT( reply );

    // we got a zip file, save it to a temporary file, then unzip it to our destination data dir
    if ( reply->error() == QNetworkReply::NoError )
    {
        QTemporaryFile f( QDir::tempPath() + QDir::separator() + "tomahawkattica_XXXXXX.zip" );
        if ( !f.open() )
        {
            tLog() << "Failed to write zip file to temp file:" << f.fileName();
            return;
        }
        f.write( reply->readAll() );
        f.close();

        QString resolverId = reply->property( "resolverId" ).toString();
        QDir dir( extractPayload( f.fileName(), resolverId ) );
        QString resolverPath = dir.absoluteFilePath( m_resolverStates[ resolverId ].scriptPath );

        if ( !resolverPath.isEmpty() )
        {
            // update with absolute, not relative, path
            m_resolverStates[ resolverId ].scriptPath = resolverPath;

            if ( reply->property( "createAccount" ).toBool() )
            {
                // Do the install / add to tomahawk
                Tomahawk::Accounts::Account* resolver = Tomahawk::Accounts::ResolverAccountFactory::createFromPath( resolverPath, "resolveraccount", true );
                Tomahawk::Accounts::AccountManager::instance()->addAccount( resolver );
                TomahawkSettings::instance()->addAccount( resolver->accountId() );
            }

            m_resolverStates[ resolverId ].state = Installed;
            TomahawkSettingsGui::instanceGui()->setAtticaResolverStates( m_resolverStates );
            emit resolverInstalled( resolverId );
            emit resolverStateChanged( resolverId );
        }
    }
    else
    {
        tLog() << "Failed to download attica payload...:" << reply->errorString();
    }
}


QString
AtticaManager::extractPayload( const QString& filename, const QString& resolverId ) const
{
    // uses QuaZip to extract the temporary zip file to the user's tomahawk data/resolvers directory
    QuaZip zipFile( filename );
    if ( !zipFile.open( QuaZip::mdUnzip ) )
    {
        tLog() << "Failed to QuaZip open:" << zipFile.getZipError();
        return QString();
    }

    if ( !zipFile.goToFirstFile() )
    {
        tLog() << "Failed to go to first file in zip archive: " << zipFile.getZipError();
        return QString();
    }

    QDir resolverDir = TomahawkUtils::appDataDir();
    if ( !resolverDir.mkpath( QString( "atticaresolvers/%1" ).arg( resolverId ) ) )
    {
        tLog() << "Failed to mkdir resolver save dir: " << TomahawkUtils::appDataDir().absoluteFilePath( QString( "atticaresolvers/%1" ).arg( resolverId ) );
        return QString();
    }
    resolverDir.cd( QString( "atticaresolvers/%1" ).arg( resolverId ) );
    tDebug() << "Installing resolver to:" << resolverDir.absolutePath();

    QuaZipFile fileInZip( &zipFile );
    do
    {
        QuaZipFileInfo info;
        zipFile.getCurrentFileInfo( &info );

        if ( !fileInZip.open( QIODevice::ReadOnly ) )
        {
            tLog() << "Failed to open file inside zip archive:" << info.name << zipFile.getZipName() << "with error:" << zipFile.getZipError();
            continue;
        }

        QFile out( resolverDir.absoluteFilePath( fileInZip.getActualFileName() ) );

        QStringList parts = fileInZip.getActualFileName().split( "/" );
        if ( parts.size() > 1 )
        {
            QStringList dirs = parts.mid( 0, parts.size() - 1 );
            QString dirPath = dirs.join( "/" ); // QDir translates / to \ internally if necessary
            resolverDir.mkpath( dirPath );
        }

        // make dir if there is one needed
        QDir d( fileInZip.getActualFileName() );

        tDebug() << "Writing to output file..." << out.fileName();
        if ( !out.open( QIODevice::WriteOnly ) )
        {
            tLog() << "Failed to open resolver extract file:" << out.errorString() << info.name;
            continue;
        }


        out.write( fileInZip.readAll() );
        out.close();
        fileInZip.close();

    } while ( zipFile.goToNextFile() );

    return resolverDir.absolutePath();
}


void
AtticaManager::uninstallResolver( const QString& pathToResolver )
{
    // when is this used? find and fix
    Q_ASSERT(false);

    // User manually removed a resolver not through attica dialog, simple remove
    QRegExp r( ".*([^/]*)/contents/code/main.js" );
    r.indexIn( pathToResolver );
    const QString& atticaId = r.cap( 1 );
    tDebug() << "Got resolver ID to remove:" << atticaId;
    if ( !atticaId.isEmpty() ) // this is an attica-installed resolver, mark as uninstalled
    {
        foreach ( const Content& resolver, m_resolvers )
        {
            if ( resolver.id() == atticaId ) // this is the one
            {
                m_resolverStates[ atticaId ].state = Uninstalled;
                TomahawkSettingsGui::instanceGui()->setAtticaResolverState( atticaId, Uninstalled );

                doResolverRemove( atticaId );
            }
        }
    }
}


void
AtticaManager::uninstallResolver( const Content& resolver )
{
    if ( m_resolverStates[ resolver.id() ].state != Upgrading )
    {
        emit resolverUninstalled( resolver.id() );
        emit resolverStateChanged( resolver.id() );

        m_resolverStates[ resolver.id() ].state = Uninstalled;
        TomahawkSettingsGui::instanceGui()->setAtticaResolverState( resolver.id(), Uninstalled );

        // remove account as well
        QList< Tomahawk::Accounts::Account* > accounts = Tomahawk::Accounts::AccountManager::instance()->accounts( Tomahawk::Accounts::ResolverType );
        foreach ( Tomahawk::Accounts::Account* account, accounts )
        {
            if ( Tomahawk::Accounts::AtticaResolverAccount* atticaAccount = qobject_cast< Tomahawk::Accounts::AtticaResolverAccount* >( account ) )
            {
                if ( atticaAccount->atticaId() == resolver.id() ) // this is the account we want to remove
                {
                    Tomahawk::Accounts::AccountManager::instance()->removeAccount( atticaAccount );
                }
            }
        }
    }

    doResolverRemove( resolver.id() );
}


void
AtticaManager::doResolverRemove( const QString& id ) const
{
    // uninstalling is easy... just delete it! :)
    QDir resolverDir = TomahawkUtils::appDataDir();
    if ( !resolverDir.cd( QString( "atticaresolvers/%1" ).arg( id ) ) )
        return;

    if ( id.isEmpty() )
        return;

    // sanity check
    if ( !resolverDir.absolutePath().contains( "atticaresolvers" ) ||
        !resolverDir.absolutePath().contains( id ) )
        return;

    TomahawkUtils::removeDirectory( resolverDir.absolutePath() );
}
