/*******************************************************************************************************
 DkFileInfo.cpp
 Created on: 04.28.2025

nomacs is a fast and small image viewer with the capability of synchronizing multiple instances

Copyright (C) 2025 Scrubs <scrubbbbs@gmail.com>

This file is part of nomacs.

nomacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

nomacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

 *******************************************************************************************************/
#include "DkFileInfo.h"

#include "DkSettings.h"
#include "DkTimer.h"
#include "DkUtils.h"

#ifdef WITH_LIBARCHIVE
#include "ArchiveDecompressor.h"
#endif

#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QRegularExpression>
#include <QStringBuilder>

#ifdef WITH_QUAZIP
#include <quazip/JlCompress.h>
#endif

namespace nmc
{
#ifdef WITH_QUAZIP

// Delimiter for zipfile+member encoded path
// - this is the same marker as KIO
// - the slash at the end makes QFileInfo::fileName(),
//   etc compatible with encoded paths
// - note: would prefer to scope to class but breaks plugins linking on Qt5/gcc
static constexpr QStringView ZipMarker = u"#/";

DkFileInfo::ZipData::ZipData(const QString &encodedFilePath)
{
    qsizetype index = encodedFilePath.indexOf(ZipMarker);
    if (index > 0) {
        mIsMember = true;
        mZipFilePath = encodedFilePath.mid(0, index);
        mZipMemberPath = encodedFilePath.mid(index + ZipMarker.length());
    }
}

DkFileInfo::ZipData::ZipData(const QString &zipFilePath, const QuaZipFileInfo64 &info)
{
    mIsMember = true;
    mZipFilePath = zipFilePath;
    mZipMemberPath = info.name;
    setMetaData(info);
}

void DkFileInfo::ZipData::setMetaData(const QuaZipFileInfo64 &info)
{
    mDecompressedSize = info.uncompressedSize;

    // support for these dates is mixed so we will try the various options
    // the extra data fields first for modtime since they have higher resolution
    mModified = info.getNTFSmTime().toLocalTime();
    if (!mModified.isValid())
        mModified = info.getExtTime(info.extra, QUAZIP_EXTRA_EXT_MOD_TIME_FLAG).toLocalTime();
    if (!mModified.isValid())
        mModified = info.dateTime;

    mCreated = info.getNTFScTime().toLocalTime();
    if (!mCreated.isValid())
        mCreated = info.getExtTime(info.extra, QUAZIP_EXTRA_EXT_CR_TIME_FLAG).toLocalTime();

    mHasMetaData = true;
}

QString DkFileInfo::ZipData::encodePath(const QString &zipFilePath, const QString &memberPath)
{
    Q_ASSERT(!memberPath.startsWith('/'));
    Q_ASSERT(QFileInfo(zipFilePath).absoluteFilePath() == zipFilePath);

    return zipFilePath % ZipMarker % memberPath;
}

void DkFileInfo::ZipData::readMetaData()
{
    // FIXME: this is a bit slow on large zipfiles, readZipArchive() could cache it,
    // or it can be eliminated by passing DkFileInfo around instead of paths
    qDebug() << "[FileInfo] zip: slow metadata parsing:" << mZipMemberPath;

    QuaZip zip(mZipFilePath);
    if (!zip.open(QuaZip::mdUnzip)) {
        qWarning() << "[FileInfo] zip: open failed:" << mZipFilePath << zip.getZipError();
        return;
    }

    if (!zip.setCurrentFile(mZipMemberPath)) {
        qWarning() << "[FileInfo] zip: locate failed:" << mZipFilePath << zip.getZipError();
        return;
    }

    QuaZipFileInfo64 info;
    if (!zip.getCurrentFileInfo(&info)) {
        qWarning() << "[FileInfo] zip: decompress failed:" << mZipFilePath << zip.getZipError();
        return;
    }

    setMetaData(info);
}
#endif

#ifdef WITH_LIBARCHIVE

static constexpr QStringView ArchiveMarker = u"&/";

DkFileInfo::ArchiveData::ArchiveData(const QString &encodedFilePath)
{
    qsizetype index = encodedFilePath.indexOf(ArchiveMarker);
    if (index > 0) {
        mIsMember = true;
        mArchiveFilePath = encodedFilePath.mid(0, index);
        mArchiveMemberPath = encodedFilePath.mid(index + ArchiveMarker.length());
    }
}

DkFileInfo::ArchiveData::ArchiveData(const QString &archiveFilePath, const ArchiveCompressor::EntryInfo &info)
{
    mIsMember = true;
    mArchiveFilePath = archiveFilePath;
    mArchiveMemberPath = QString::fromUtf8(info.path);
    setMetaData(info);
}

QString DkFileInfo::ArchiveData::encodePath(const QString &filePath, const QString &memberPath)
{
    Q_ASSERT(!memberPath.startsWith('/'));
    Q_ASSERT(QFileInfo(filePath).absoluteFilePath() == filePath);

    return filePath % ArchiveMarker % memberPath;
}

void DkFileInfo::ArchiveData::readMetaData()
{
    if (!mIsMember || mHasMetaData)
        return;

    ArchiveCompressor ar(mArchiveFilePath.toStdString());
    if (!ar.lastError().empty()) {
        qWarning() << "[FileInfo] archive: open failed:" << mArchiveFilePath
                   << QString::fromStdString(ar.lastError());
        return;
    }

    for (auto it = ar.begin(); it != ar.end(); ++it) {
        if (QString::fromUtf8(it->path()) == mArchiveMemberPath) {
            setMetaData(it->info());
            return;
        }
    }

    qWarning() << "[FileInfo] archive: member not found:" << mArchiveMemberPath;
}

void DkFileInfo::ArchiveData::clearMetaData()
{
    mHasMetaData = false;
    mContentLoaded = false;
    mContent.clear();
}

bool DkFileInfo::ArchiveData::ensureContent() const
{
    if (!mIsMember)
        return false;
    if (mContentLoaded)
        return true;

    ArchiveCompressor ar(mArchiveFilePath.toStdString());
    if (!ar.lastError().empty()) {
        qWarning() << "[FileInfo] archive: open failed:" << mArchiveFilePath
                   << QString::fromStdString(ar.lastError());
        return false;
    }

    std::vector<uint8_t> content;
    if (!ar.extractToMemory(mArchiveMemberPath.toStdString(), content)) {
        qWarning() << "[FileInfo] archive: extract failed:" << mArchiveMemberPath
                   << QString::fromStdString(ar.lastError());
        return false;
    }

    mContent = std::move(content);
    mContentLoaded = true;
    return true;
}

void DkFileInfo::ArchiveData::setMetaData(const ArchiveCompressor::EntryInfo &info)
{
    mSize = info.size;

    const auto toDateTime = [](const ArchiveCompressor::TimeInfo &time) {
        if (!time.isSet())
            return QDateTime{};
        const qint64 msecs = static_cast<qint64>(time.sec) * 1000 + time.nsec / 1000000;
        return QDateTime::fromMSecsSinceEpoch(msecs, Qt::UTC).toLocalTime();
    };

    mModified = toDateTime(info.mtime);
    if (!mModified.isValid())
        mModified = toDateTime(info.ctime);

    mCreated = toDateTime(info.birthtime);
    if (!mCreated.isValid())
        mCreated = toDateTime(info.ctime);

    mHasMetaData = true;
}

#endif // WITH_LIBARCHIVE

DkFileInfo::SharedData::SharedData(const QString &path)
    : mFileInfo(path)
#ifdef WITH_QUAZIP
    , mZipData(mFileInfo.absoluteFilePath())
#endif
#ifdef WITH_LIBARCHIVE
    , mArData(mFileInfo.absoluteFilePath())
#endif
{
#ifdef WITH_QUAZIP
    if (mZipData.isZipMember())
        mContainerInfo = QFileInfo(mZipData.zipFilePath());
#endif
#ifdef WITH_LIBARCHIVE
    if (mArData.isMember())
        mContainerInfo = QFileInfo(mArData.filePath());
#endif
}

DkFileInfo::SharedData::SharedData(const QFileInfo &info)
    : mFileInfo(info)
#ifdef WITH_QUAZIP
    , mZipData(mFileInfo.absoluteFilePath())
#endif
#ifdef WITH_LIBARCHIVE
    , mArData(mFileInfo.absoluteFilePath())
#endif
{
#ifdef WITH_QUAZIP
    if (mZipData.isZipMember())
        mContainerInfo = QFileInfo(mZipData.zipFilePath());
#endif
#ifdef WITH_LIBARCHIVE
    if (mArData.isMember())
        mContainerInfo = QFileInfo(mArData.filePath());
#endif
}

#ifdef WITH_QUAZIP

DkFileInfo::SharedData::SharedData(const QString &zipPath, const QuaZipFileInfo64 &info)
    : mFileInfo(ZipData::encodePath(zipPath, info.name))
    , mZipData(zipPath, info)
    , mContainerInfo(zipPath)
{
}

#endif // WITH_QUAZIP

#ifdef WITH_LIBARCHIVE

DkFileInfo::SharedData::SharedData(const QString &archivePath, const ArchiveCompressor::EntryInfo &info)
    : mFileInfo(ArchiveData::encodePath(archivePath, QString::fromUtf8(info.path)))
    , mArData(archivePath, info)
    , mContainerInfo(archivePath)
{
}

#endif // WITH_LIBARCHIVE

DkFileInfo::DkFileInfo(const QString &path)
    : d(new SharedData(path))
{
}

DkFileInfo::DkFileInfo(const QFileInfo &info)
    : d(new SharedData(info))
{
}

DkFileInfo::DkFileInfo(SharedData *shared)
    : d(shared)
{
}

bool DkFileInfo::operator==(const DkFileInfo &other) const
{
    return path() == other.path();
}

bool DkFileInfo::isContainer(const QFileInfo &fileInfo)
{
    const QString &rawFilters = DkSettingsManager::param().app().containerRawFilters; // "*.zip *.cbz *.docx"
    QString suffix = fileInfo.suffix();
    return !suffix.isEmpty() && rawFilters.contains(suffix, Qt::CaseInsensitive) && fileInfo.isFile();
}

#ifdef WITH_QUAZIP

bool DkFileInfo::isFromZip() const
{
    return d->mZipData.isZipMember();
}

bool DkFileInfo::isZipFile() const
{
    if (isFromZip() || isArchiveFile())
        return false;

    // never send a rar through QuaZip
    if (d->mFileInfo.suffix().compare(u"rar", Qt::CaseInsensitive) == 0)
        return false;

    return isContainer(d->mFileInfo);
}

QString DkFileInfo::pathInZip() const
{
    return d->mZipData.zipMemberPath();
}

void DkFileInfo::readZipMetaData() const
{
    // would like to do this lazily, since it is somewhat costly and
    // often unused (readDirectory() already includes it)
    // we have to break const, but this is fine as dereference will
    // detach the shared data (deep copy) if needed.
    if (isFromZip() && !d->mZipData.hasMetaData()) {
        auto *dataPtr = const_cast<QSharedDataPointer<SharedData> *>(&d);
        (*dataPtr)->mZipData.readMetaData();
    }
}

#endif

bool DkFileInfo::isFromArchive() const
{
#ifdef WITH_LIBARCHIVE
    return d->mArData.isMember();
#else
    return false;
#endif
}

bool DkFileInfo::isArchiveFile() const
{
#ifdef WITH_LIBARCHIVE
    if (isFromArchive())
        return false;
    return d->mFileInfo.suffix().compare(u"rar", Qt::CaseInsensitive) == 0 && isContainer(d->mFileInfo);
#else
    return false;
#endif
}

QString DkFileInfo::pathInArchive() const
{
#ifdef WITH_LIBARCHIVE
    return d->mArData.memberPath();
#else
    return {};
#endif
}

bool DkFileInfo::isFromContainer() const
{
    return isFromZip() || isFromArchive();
}

bool DkFileInfo::isContainerFile() const
{
    return isZipFile() || isArchiveFile();
}

QString DkFileInfo::pathInContainer() const
{
    if (isFromZip())
        return pathInZip();
    if (isFromArchive())
        return pathInArchive();
    return {};
}

void DkFileInfo::readArchiveMetaData() const
{
#ifdef WITH_LIBARCHIVE
    // same lazy pattern as readZipMetaData(): dereferencing detaches if needed
    if (isFromArchive() && !d->mArData.hasMetaData()) {
        auto *dataPtr = const_cast<QSharedDataPointer<SharedData> *>(&d);
        (*dataPtr)->mArData.readMetaData();
    }
#endif
}

std::unique_ptr<QIODevice> DkFileInfo::getIODevice() const
{
    std::unique_ptr<QIODevice> io;

    if (isFromZip()) {
#ifdef WITH_QUAZIP
        io = std::make_unique<QuaZipFile>(d->mZipData.zipFilePath(), d->mZipData.zipMemberPath());
#endif
    } else if (isFromArchive()) {
#ifdef WITH_LIBARCHIVE
        auto *dataPtr = const_cast<QSharedDataPointer<SharedData> *>(&d);
        auto &archiveData = (*dataPtr)->mArData;
        if (!archiveData.ensureContent()) {
            qWarning() << "[FileInfo] archive: could not read member:" << path();
            return {};
        }

        const auto &content = archiveData.content();
        auto buffer = std::make_unique<QBuffer>();
        buffer->setData(QByteArray(reinterpret_cast<const char *>(content.data()),
                                   static_cast<qsizetype>(content.size())));
        io = std::move(buffer);
#endif
    } else {
        io = std::make_unique<QFile>(path());
    }

    Q_ASSERT(io != nullptr);

    if (!io->open(QIODevice::ReadOnly)) {
        qWarning() << "[FileInfo] failed to open i/o" << path() << io->errorString();
        io = {};
    }

    return io;
}

QString DkFileInfo::path() const
{
    return d->mFileInfo.absoluteFilePath();
}

QString DkFileInfo::dirPath() const
{
    if (isFromContainer())
        return containerInfo().absoluteFilePath();
    else
        return d->mFileInfo.absolutePath();
}

bool DkFileInfo::exists() const
{
    return containerInfo().exists();
}

bool DkFileInfo::isFile() const
{
    return isContainerFile() ? false : containerInfo().isFile();
}

bool DkFileInfo::isDir() const
{
    return isContainerFile() ? true : d->mFileInfo.isDir();
}

bool DkFileInfo::isReadable() const
{
    return containerInfo().isReadable();
}

QString DkFileInfo::fileName() const
{
    return d->mFileInfo.fileName();
}

QString DkFileInfo::suffix() const
{
    return d->mFileInfo.suffix();
}

QString DkFileInfo::baseName() const
{
    return d->mFileInfo.baseName();
}

QDateTime DkFileInfo::birthTime() const
{
    readZipMetaData();
    readArchiveMetaData();
#ifdef WITH_QUAZIP
    if (isFromZip())
        return d->mZipData.birthTime();
#endif
#ifdef WITH_LIBARCHIVE
    if (isFromArchive())
        return d->mArData.birthTime();
#endif
    return d->mFileInfo.birthTime();
}

QDateTime DkFileInfo::lastModified() const
{
    readZipMetaData();
    readArchiveMetaData();
#ifdef WITH_QUAZIP
    if (isFromZip())
        return d->mZipData.lastModified();
#endif
#ifdef WITH_LIBARCHIVE
    if (isFromArchive())
        return d->mArData.lastModified();
#endif
    return d->mFileInfo.lastModified();
}

QDateTime DkFileInfo::lastRead() const
{
    return containerInfo().lastRead();
}

bool DkFileInfo::isModified()
{
    QFileInfo &info = isFromContainer() ? d->mContainerInfo : d->mFileInfo;

    QDateTime prevModified = info.lastModified();
    bool prevExists = info.exists();

    // do not use refresh() as it won't see exists() change
    info = QFileInfo(info.absoluteFilePath());

    bool modified = info.exists() != prevExists || info.lastModified() != prevModified;
#ifdef WITH_QUAZIP
    if (modified && isFromZip())
        d->mZipData.zipFileModified(); // force reload of zip metadata
#endif
#ifdef WITH_LIBARCHIVE
    if (modified && isFromArchive())
        d->mArData.clearMetaData();
#endif
    return modified;
}

QString DkFileInfo::owner() const
{
    return containerInfo().owner();
}

uint DkFileInfo::ownerId() const
{
    return containerInfo().ownerId();
}

QString DkFileInfo::group() const
{
    return containerInfo().group();
}

QFile::Permissions DkFileInfo::permissions() const
{
    return containerInfo().permissions();
}

bool DkFileInfo::permission(QFileDevice::Permissions flags) const
{
    return containerInfo().permission(flags);
}

bool DkFileInfo::isSymLink() const
{
    return isFromContainer() ? false : d->mFileInfo.isSymLink();
}

bool DkFileInfo::resolveSymLink()
{
    if (!isSymLink())
        return false;

    *this = DkFileInfo(d->mFileInfo.symLinkTarget());
    return exists();
}

QString DkFileInfo::symLinkTarget() const
{
    return containerInfo().symLinkTarget();
}

qint64 DkFileInfo::size() const
{
    readZipMetaData();
    readArchiveMetaData();
#ifdef WITH_QUAZIP
    if (isFromZip())
        return d->mZipData.size();
#endif
#ifdef WITH_LIBARCHIVE
    if (isFromArchive())
        return d->mArData.size();
#endif
    return d->mFileInfo.size();
}

void DkFileInfo::stat()
{
    if (isFromContainer())
        d->mContainerInfo.stat();
    else
        d->mFileInfo.stat();

    readZipMetaData();
    readArchiveMetaData();
}

const QFileInfo &DkFileInfo::containerInfo() const
{
    return isFromContainer() ? d->mContainerInfo : d->mFileInfo;
}

// remove files that have the same name but different extension
// - assume list came from a single directory (no recursive scan)
// - output is sorted randomly
DkFileInfoList filterDuplicateNames(const DkFileInfoList &list)
{
    // extension we will prefer to choose if there are duplicate file names
    QString preferredExtension = DkSettingsManager::param().resources().preferredExtension;
    preferredExtension = preferredExtension.replace("*.", "");

    QHash<QString, DkFileInfo> dups;
    for (auto &fi : list) {
        const QString name = fi.baseName();
        const QString suffix = fi.suffix().toLower();

        auto it = dups.find(name);
        if (it == dups.end())
            dups.insert(name, fi);
        else if (it->suffix().compare(preferredExtension, Qt::CaseInsensitive) != 0 && //
                 suffix.compare(preferredExtension, Qt::CaseInsensitive) == 0)
            dups[name] = fi;
    }

    return dups.values();
}

// filter file name matching expression
DkFileInfoList filterFileName(const QRegularExpression &regExp, const DkFileInfoList &list)
{
    DkFileInfoList result;
    for (auto &fi : list)
        if (fi.fileName().contains(regExp))
            result.append(fi);
    return result;
}

// filter file name containing word
DkFileInfoList filterFileName(const QString &word, const DkFileInfoList &list)
{
    DkFileInfoList result;
    for (auto &fi : list)
        if (fi.fileName().contains(word, Qt::CaseInsensitive))
            result.append(fi);
    return result;
}

// filter list with query string
// - keywords separated by space " " (case-insensitive)
// - regular expression (case-sensitive)
// - glob pattern (anchored, case-insensitive)
DkFileInfoList filterInfoList(const QString &query, const DkFileInfoList &list)
{
    QStringList keywords = query.split(" ");
    DkFileInfoList result = list;

    for (int idx = 0; idx < keywords.size(); idx++) {
        // Detect and correct special case where a space is leading or trailing the search term - this should be
        // significant
        if (idx == 0 && keywords.size() > 1 && keywords[idx].size() == 0)
            keywords[idx] = " " + keywords[idx + 1];
        if (idx == keywords.size() - 1 && keywords.size() > 2 && keywords[idx].size() == 0)
            keywords[idx] = keywords[idx - 1] + " ";
        // The queries will be repeated, but this is okay - it will just be matched both with and without the space.
        // result = result.filter(queries[idx], Qt::CaseInsensitive);
        result = filterFileName(keywords[idx], result);
    }

    if (result.empty()) {
        // regexp, case sensitive
        QRegularExpression regExp(query);
        if (regExp.isValid())
            result = filterFileName(regExp, list);

        // globbing, anchored, case-insensitive
        if (result.isEmpty()) {
            regExp = QRegularExpression(QRegularExpression::wildcardToRegularExpression(query),
                                        QRegularExpression::CaseInsensitiveOption);
            if (regExp.isValid())
                result = filterFileName(regExp, list);
        }
    }

    return result;
}

#ifdef WITH_QUAZIP

DkFileInfoList DkFileInfo::readZipArchive(const QString &zipPath)
{
    QuaZip zip(zipPath);
    if (!zip.open(QuaZip::mdUnzip)) {
        qWarning() << "[FileInfo] zip: open failed:" << zipPath << zip.getZipError();
        zip.getZipError();
        return {};
    }

    DkFileInfoList fileInfoList;
    QuaZipFileInfo64 info;
    int idx = 0;

    for (bool more = zip.goToFirstFile(); more; more = zip.goToNextFile()) {
        if (!zip.getCurrentFileInfo(&info)) {
            qWarning() << "[FileInfo] zip: getCurrentFile failed @index:" << idx << zipPath << zip.getZipError();
            continue;
        }

        // ignore MacOS metadata, could be parsed with adouble interface from netatalk
        if (info.name.startsWith("__MACOSX/._"))
            continue;

        // ignore directories
        if (info.name.endsWith('/'))
            continue;

        fileInfoList += DkFileInfo(new SharedData(zipPath, info));
        idx++;
    }

    return fileInfoList;
}
#endif

#ifdef WITH_LIBARCHIVE

DkFileInfoList DkFileInfo::readArchive(const QString &arPath)
{
    const QString archivePath = QFileInfo(arPath).absoluteFilePath();
    ArchiveCompressor ar(archivePath.toStdString());
    if (!ar.lastError().empty()) {
        qWarning() << "[FileInfo] archive: open failed:" << archivePath << QString::fromStdString(ar.lastError());
        return {};
    }

    DkFileInfoList ret;
    for (const auto &entry : ar) {
        const auto &info = entry.info();

        // ignore directories, links, macOS metadata, and entries without a usable path
        if (info.path.empty() || info.path.back() == '/' || entry.isDirectory())
            continue;
        if (!info.symlink.empty() || !info.hardlink.empty())
            continue;
        if (info.path.rfind("__MACOSX/._", 0) == 0)
            continue;

        // password protected members are not supported yet; do not list them
        if (info.encrypted) {
            qWarning() << "[FileInfo] archive: skipping encrypted entry:" << QString::fromUtf8(info.path);
            continue;
        }

        // content is extracted lazily by SharedData/getIODevice()
        ret += DkFileInfo(new SharedData(archivePath, info));
    }

    return ret;
}

#endif // WITH_LIBARCHIVE

DkFileInfoList DkFileInfo::readDirectory(const QString &dirPath, const QString &nameFilter)
{
    DkTimer dt;

    if (dirPath.isEmpty())
        return {};

    DkFileInfoList unfiltered;

    const auto dkFileInfo = DkFileInfo(dirPath);
#ifdef WITH_QUAZIP
    if (dkFileInfo.isZipFile())
        unfiltered = readZipArchive(dirPath);
    else
#endif
#ifdef WITH_LIBARCHIVE
    if (dkFileInfo.isArchiveFile())
        unfiltered = readArchive(dirPath);
    else
#endif
    {
        // all files, unfiltered, unsorted
        const QFileInfoList list = QDir(dirPath).entryInfoList(QDir::Files, QDir::NoSort);
        unfiltered.reserve(list.count());
        for (auto &fileInfo : list)
            unfiltered.append(DkFileInfo(fileInfo));
    }

    // seems better to use a hashtable here; ~50 extensions are possible without kimageformats
    const QStringList &fileFilters = DkSettingsManager::param().app().browseFilters;
    const QStringList &containerFilters = DkSettingsManager::param().app().containerRawFilters.split(u' ');
    QSet<QString> suffixes;
    for (const QString &filter : fileFilters) {
        if (containerFilters.contains(filter)) {
            continue;
        }
        suffixes.insert(QString(filter).replace("*.", ""));
    }

    DkFileInfoList filtered;

    // filter by suffix
    for (auto &fileInfo : std::as_const(unfiltered)) {
        DkFileInfo tmpInfo = fileInfo;
        if (tmpInfo.isSymLink() && !tmpInfo.resolveSymLink())
            continue;

        const QString suffix = tmpInfo.suffix().toLower();
        if (suffix.isEmpty() && !DkUtils::isLoadableByContent(tmpInfo)) // reads file header, maybe slow
            continue;
        else if (!suffixes.contains(suffix))
            continue;

        filtered.append(fileInfo);
    }

    // filter with keywords, regexp, or glob
    if (!nameFilter.isEmpty())
        filtered = filterInfoList(nameFilter, filtered);

    // filter duplicate basenames
    if (DkSettingsManager::param().resources().filterDuplicats)
        filtered = filterDuplicateNames(filtered);

    qDebug() << "[readDirectory] list directory:" << dirPath << dt;

    return filtered;
}
}
