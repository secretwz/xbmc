/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "TextureCache.h"
#include "TextureCacheJob.h"
#include "filesystem/File.h"
#include "profiles/ProfilesManager.h"
#include "threads/SingleLock.h"
#include "utils/Crc32.h"
#include "settings/AdvancedSettings.h"
#include "utils/log.h"
#include "utils/URIUtils.h"
#include "utils/StringUtils.h"
#include "URL.h"
#include "utils/StringUtils.h"

using namespace XFILE;

CTextureCache &CTextureCache::Get()
{
  static CTextureCache s_cache;
  return s_cache;
}

CTextureCache::CTextureCache() : CJobQueue(false, 1, CJob::PRIORITY_LOW_PAUSABLE)
{
}

CTextureCache::~CTextureCache()
{
}

void CTextureCache::Initialize()
{
  CSingleLock lock(m_databaseSection);
  if (!m_database.IsOpen())
    m_database.Open();
}

void CTextureCache::Deinitialize()
{
  CancelJobs();
  CSingleLock lock(m_databaseSection);
  m_database.Close();
}

bool CTextureCache::IsCachedImage(const CStdString &url) const
{
  if (url != "-" && !CURL::IsFullPath(url))
    return true;
  if (URIUtils::IsInPath(url, "special://skin/") ||
      URIUtils::IsInPath(url, "androidapp://")   ||
      URIUtils::IsInPath(url, CProfilesManager::Get().GetThumbnailsFolder()))
    return true;
  return false;
}

bool CTextureCache::HasCachedImage(const CStdString &url)
{
  CTextureDetails details;
  CStdString cachedImage(GetCachedImage(url, details));
  return (!cachedImage.empty() && cachedImage != url);
}

CStdString CTextureCache::GetCachedImage(const CStdString &image, CTextureDetails &details, bool trackUsage)
{
  CStdString url = CTextureUtils::UnwrapImageURL(image);

  if (IsCachedImage(url))
    return url;

  // lookup the item in the database
  if (GetCachedTexture(url, details))
  {
    if (trackUsage)
      IncrementUseCount(details);
    return GetCachedPath(details.file);
  }
  return "";
}

bool CTextureCache::CanCacheImageURL(const CURL &url)
{
  return (url.GetUserName().empty() || url.GetUserName() == "music");
}

CStdString CTextureCache::CheckCachedImage(const CStdString &url, bool returnDDS, bool &needsRecaching)
{
  CTextureDetails details;
  CStdString path(GetCachedImage(url, details, true));
  needsRecaching = !details.hash.empty();
  if (!path.empty())
  {
    if (!needsRecaching && returnDDS && !URIUtils::IsInPath(url, "special://skin/")) // TODO: should skin images be .dds'd (currently they're not necessarily writeable)
    { // check for dds version
      CStdString ddsPath = URIUtils::ReplaceExtension(path, ".dds");
      if (CFile::Exists(ddsPath))
        return ddsPath;
      if (g_advancedSettings.m_useDDSFanart)
        AddJob(new CTextureDDSJob(path));
    }
    return path;
  }
  return "";
}

void CTextureCache::BackgroundCacheImage(const CStdString &url)
{
  CTextureDetails details;
  CStdString path(GetCachedImage(url, details));
  if (!path.empty() && details.hash.empty())
    return; // image is already cached and doesn't need to be checked further

  // needs (re)caching
  AddJob(new CTextureCacheJob(CTextureUtils::UnwrapImageURL(url), details.hash));
}

bool CTextureCache::CacheImage(const CStdString &image, CTextureDetails &details)
{
  CStdString path = GetCachedImage(image, details);
  if (path.empty()) // not cached
    path = CacheImage(image, NULL, &details);

  return !path.empty();
}

CStdString CTextureCache::CacheImage(const CStdString &image, CBaseTexture **texture, CTextureDetails *details)
{
  CStdString url = CTextureUtils::UnwrapImageURL(image);
  CSingleLock lock(m_processingSection);
  if (m_processinglist.find(url) == m_processinglist.end())
  {
    m_processinglist.insert(url);
    lock.Leave();
    // cache the texture directly
    CTextureCacheJob job(url);
    bool success = job.CacheTexture(texture);
    OnCachingComplete(success, &job);
    if (success && details)
      *details = job.m_details;
    return success ? GetCachedPath(job.m_details.file) : "";
  }
  lock.Leave();

  // wait for currently processing job to end.
  while (true)
  {
    m_completeEvent.WaitMSec(1000);
    {
      CSingleLock lock(m_processingSection);
      if (m_processinglist.find(url) == m_processinglist.end())
        break;
    }
  }
  CTextureDetails tempDetails;
  if (!details)
    details = &tempDetails;
  return GetCachedImage(url, *details, true);
}

void CTextureCache::ClearCachedImage(const CStdString &url, bool deleteSource /*= false */)
{
  // TODO: This can be removed when the texture cache covers everything.
  CStdString path = deleteSource ? url : "";
  CStdString cachedFile;
  if (ClearCachedTexture(url, cachedFile))
    path = GetCachedPath(cachedFile);
  if (CFile::Exists(path))
    CFile::Delete(path);
  path = URIUtils::ReplaceExtension(path, ".dds");
  if (CFile::Exists(path))
    CFile::Delete(path);
}

bool CTextureCache::ClearCachedImage(int id)
{
  CStdString cachedFile;
  if (ClearCachedTexture(id, cachedFile))
  {
    cachedFile = GetCachedPath(cachedFile);
    if (CFile::Exists(cachedFile))
      CFile::Delete(cachedFile);
    cachedFile = URIUtils::ReplaceExtension(cachedFile, ".dds");
    if (CFile::Exists(cachedFile))
      CFile::Delete(cachedFile);
    return true;
  }
  return false;
}

bool CTextureCache::GetCachedTexture(const CStdString &url, CTextureDetails &details)
{
  CSingleLock lock(m_databaseSection);
  return m_database.GetCachedTexture(url, details);
}

bool CTextureCache::AddCachedTexture(const CStdString &url, const CTextureDetails &details)
{
  CSingleLock lock(m_databaseSection);
  return m_database.AddCachedTexture(url, details);
}

void CTextureCache::IncrementUseCount(const CTextureDetails &details)
{
  static const size_t count_before_update = 100;
  CSingleLock lock(m_useCountSection);
  m_useCounts.reserve(count_before_update);
  m_useCounts.push_back(details);
  if (m_useCounts.size() >= count_before_update)
  {
    AddJob(new CTextureUseCountJob(m_useCounts));
    m_useCounts.clear();
  }
}

bool CTextureCache::SetCachedTextureValid(const CStdString &url, bool updateable)
{
  CSingleLock lock(m_databaseSection);
  return m_database.SetCachedTextureValid(url, updateable);
}

bool CTextureCache::ClearCachedTexture(const CStdString &url, CStdString &cachedURL)
{
  CSingleLock lock(m_databaseSection);
  return m_database.ClearCachedTexture(url, cachedURL);
}

bool CTextureCache::ClearCachedTexture(int id, CStdString &cachedURL)
{
  CSingleLock lock(m_databaseSection);
  return m_database.ClearCachedTexture(id, cachedURL);
}

CStdString CTextureCache::GetCacheFile(const CStdString &url)
{
  Crc32 crc;
  crc.ComputeFromLowerCase(url);
  CStdString hex = StringUtils::Format("%08x", (unsigned int)crc);
  CStdString hash = StringUtils::Format("%c/%s", hex[0], hex.c_str());
  return hash;
}

CStdString CTextureCache::GetCachedPath(const CStdString &file)
{
  return URIUtils::AddFileToFolder(CProfilesManager::Get().GetThumbnailsFolder(), file);
}

void CTextureCache::OnCachingComplete(bool success, CTextureCacheJob *job)
{
  if (success)
  {
    if (job->m_oldHash == job->m_details.hash)
      SetCachedTextureValid(job->m_url, job->m_details.updateable);
    else
      AddCachedTexture(job->m_url, job->m_details);
  }

  { // remove from our processing list
    CSingleLock lock(m_processingSection);
    std::set<CStdString>::iterator i = m_processinglist.find(job->m_url);
    if (i != m_processinglist.end())
      m_processinglist.erase(i);
  }

  m_completeEvent.Set();

  // TODO: call back to the UI indicating that it can update it's image...
  if (success && g_advancedSettings.m_useDDSFanart && !job->m_details.file.empty())
    AddJob(new CTextureDDSJob(GetCachedPath(job->m_details.file)));
}

void CTextureCache::OnJobComplete(unsigned int jobID, bool success, CJob *job)
{
  if (strcmp(job->GetType(), kJobTypeCacheImage) == 0)
    OnCachingComplete(success, (CTextureCacheJob *)job);
  return CJobQueue::OnJobComplete(jobID, success, job);
}

void CTextureCache::OnJobProgress(unsigned int jobID, unsigned int progress, unsigned int total, const CJob *job)
{
  if (strcmp(job->GetType(), kJobTypeCacheImage) == 0 && !progress)
  { // check our processing list
    {
      CSingleLock lock(m_processingSection);
      const CTextureCacheJob *cacheJob = (CTextureCacheJob *)job;
      std::set<CStdString>::iterator i = m_processinglist.find(cacheJob->m_url);
      if (i == m_processinglist.end())
      {
        m_processinglist.insert(cacheJob->m_url);
        return;
      }
    }
    CancelJob(job);
  }
  else
    CJobQueue::OnJobProgress(jobID, progress, total, job);
}

bool CTextureCache::Export(const CStdString &image, const CStdString &destination, bool overwrite)
{
  CTextureDetails details;
  CStdString cachedImage(GetCachedImage(image, details));
  if (!cachedImage.empty())
  {
    CStdString dest = destination + URIUtils::GetExtension(cachedImage);
    if (overwrite || !CFile::Exists(dest))
    {
      if (CFile::Cache(cachedImage, dest))
        return true;
      CLog::Log(LOGERROR, "%s failed exporting '%s' to '%s'", __FUNCTION__, cachedImage.c_str(), dest.c_str());
    }
  }
  return false;
}

bool CTextureCache::Export(const CStdString &image, const CStdString &destination)
{
  CTextureDetails details;
  CStdString cachedImage(GetCachedImage(image, details));
  if (!cachedImage.empty())
  {
    if (CFile::Cache(cachedImage, destination))
      return true;
    CLog::Log(LOGERROR, "%s failed exporting '%s' to '%s'", __FUNCTION__, cachedImage.c_str(), destination.c_str());
  }
  return false;
}
