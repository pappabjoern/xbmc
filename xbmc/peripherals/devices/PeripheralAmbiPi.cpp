/*
 *      Copyright (C) 2005-2012 Team XBMC
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
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "PeripheralAmbiPi.h"
#include "utils/log.h"

#include "Application.h"
#include "utils/Screenshot.h"
#include "cores/VideoRenderers/RenderManager.h"

#define AMBIPI_DEFAULT_PORT 20434

extern CApplication g_application;
extern CXBMCRenderManager g_renderManager;

using namespace PERIPHERALS;
using namespace AUTOPTR;

CPeripheralAmbiPi::CPeripheralAmbiPi(const PeripheralType type, const PeripheralBusType busType, const CStdString &strLocation, const CStdString &strDeviceName, int iVendorId, int iProductId) :
  CPeripheral(type, busType, strLocation, strDeviceName, iVendorId, iProductId),
  CThread("AmbiPiCapture"),
  m_bStarted(false),
  m_bIsRunning(false),
  m_pGrid(NULL),
  m_previousImageWidth(0),
  m_previousImageHeight(0),
  m_lastFrameTime(0)
{  
  m_features.push_back(FEATURE_AMBIPI);
}

CPeripheralAmbiPi::~CPeripheralAmbiPi(void)
{
  CLog::Log(LOGDEBUG, "%s - Removing AmbiPi on %s", __FUNCTION__, m_strLocation.c_str());

  StopCapture();

  delete m_pGrid;
}

void CPeripheralAmbiPi::StopCapture(void)
{
  if (IsRunning()) {
    StopThread(true);
  }
}

bool CPeripheralAmbiPi::InitialiseFeature(const PeripheralFeature feature)
{
  if (feature != FEATURE_AMBIPI || m_bStarted || !GetSettingBool("enabled")) {
    return CPeripheral::InitialiseFeature(feature);
  }

  CLog::Log(LOGDEBUG, "%s - Initialising AmbiPi on %s", __FUNCTION__, m_strLocation.c_str());

  LoadAddressFromConfiguration();
  UpdateGridFromConfiguration();  

  m_bStarted = true;
  Create();

  return CPeripheral::InitialiseFeature(feature);
}

void CPeripheralAmbiPi::LoadAddressFromConfiguration()
{ 
  CStdString portFromConfiguration = GetSettingString("port");

  int port;

  if (sscanf(portFromConfiguration.c_str(), "%d", &port) &&
      port >= 0 &&
      port <= 65535)
    m_port = port;
  else
    m_port = AMBIPI_DEFAULT_PORT;

  m_address = GetSettingString("address");
}

void CPeripheralAmbiPi::UpdateGridFromConfiguration()
{
  unsigned int width = 16 * 2;
  unsigned int height = 11 * 2;

  if (m_pGrid)
    delete m_pGrid;

  m_pGrid = new CAmbiPiGrid(width, height);  

  ConfigureCaptureSize();
}

#define WAIT_DELAY 1000

void CPeripheralAmbiPi::Process(void)
{
  {
    CSingleLock lock(m_critSection);
    m_bIsRunning = true;
  }

  ConnectToDevice();  

  m_capture = g_renderManager.AllocRenderCapture();

  g_renderManager.Capture(m_capture, m_captureWidth, m_captureHeight, CAPTUREFLAG_CONTINUOUS);
  while (!m_bStop)
  {
    m_capture->GetEvent().WaitMSec(WAIT_DELAY);
    if (m_capture->GetUserState() != CAPTURESTATE_DONE)
      continue;

    if (!ShouldProcessImage()) 
      continue;

    ProcessImage();
  }
  g_renderManager.ReleaseRenderCapture(m_capture);

  DisconnectFromDevice();

  {
    CSingleLock lock(m_critSection);
    m_bStarted = false;
    m_bIsRunning = false;
  }
}

void CPeripheralAmbiPi::ConnectToDevice()
{
  CLog::Log(LOGINFO, "%s - Connecting to AmbiPi on %s:%d", __FUNCTION__, m_address.c_str(), m_port);  

  m_connection.Disconnect();
  m_connection.Connect(m_address, m_port);
}

void CPeripheralAmbiPi::ProcessImage()
{
  GenerateDataStreamFromImage();
  SendData();
}

#define MAX_FRAMES_PER_SECOND 30
#define MIN_FRAME_DELAY (1000 / MAX_FRAMES_PER_SECOND)

bool CPeripheralAmbiPi::ShouldProcessImage()
{
  return m_connection.IsConnected();// && HasMinimumFrameTimePassed();
}

bool CPeripheralAmbiPi::HasMinimumFrameTimePassed()
{
  unsigned int now = XbmcThreads::SystemClockMillis();
  bool shouldProcess = now > m_lastFrameTime + MIN_FRAME_DELAY;

  if (shouldProcess) {
    m_lastFrameTime = now;
  }

  return shouldProcess;
}

void CPeripheralAmbiPi::GenerateDataStreamFromImage()
{
  UpdateSampleRectangles(m_captureWidth, m_captureHeight);

  m_pGrid->UpdateTilesFromImage(m_capture);
}

void CPeripheralAmbiPi::UpdateSampleRectangles(unsigned int imageWidth, unsigned int imageHeight)
{
  if (imageWidth == m_previousImageWidth || imageHeight == m_previousImageHeight)
    return;

  m_previousImageWidth = imageWidth;
  m_previousImageHeight = imageHeight;

  m_pGrid->UpdateSampleRectangles(imageWidth, imageHeight);
}

void CPeripheralAmbiPi::SendData(void) {
  TileData *pTileData = m_pGrid->GetTileData();
  try {
    m_connection.Send(pTileData->stream, pTileData->streamLength);
  } catch (std::exception) {
    CLog::Log(LOGINFO, "%s - Send exception", __FUNCTION__);  
  }
}

void CPeripheralAmbiPi::ConfigureCaptureSize()
{
  float aspectRatio = m_pGrid->GetAspectRatio();

  m_captureWidth = (100);
  m_captureHeight = (int)(m_captureWidth / aspectRatio);
}

void CPeripheralAmbiPi::OnSettingChanged(const CStdString &strChangedSetting)
{
  CLog::Log(LOGDEBUG, "%s - handling configuration change, setting: '%s'", __FUNCTION__, strChangedSetting.c_str());

  DisconnectFromDevice();
  InitialiseFeature(FEATURE_AMBIPI);
}

void CPeripheralAmbiPi::DisconnectFromDevice(void)
{
  CLog::Log(LOGDEBUG, "%s - disconnecting from the AmbiPi", __FUNCTION__);
  m_connection.Disconnect();
}

bool CPeripheralAmbiPi::IsRunning(void) const
{
  CSingleLock lock(m_critSection);
  return m_bIsRunning;
}

