/*ckwg +29
 * Copyright 2016-2017 by Kitware, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 *  * Neither the name Kitware, Inc. nor the names of any contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "MainWindow.h"

#include "ui_MainWindow.h"
#include "am_MainWindow.h"

#include "tools/BundleAdjustTool.h"
#include "tools/CanonicalTransformTool.h"
#include "tools/InitCamerasLandmarksTool.h"
#include "tools/NeckerReversalTool.h"
#include "tools/TrackFeaturesTool.h"
#include "tools/TrackFilterTool.h"

#include "AboutDialog.h"
#include "MatchMatrixWindow.h"
#include "Project.h"
#include "vtkMaptkImageDataGeometryFilter.h"
#include "vtkMaptkImageUnprojectDepth.h"
#include "vtkMaptkCamera.h"

#include <maptk/version.h>

#include <vital/io/camera_io.h>
#include <vital/io/landmark_map_io.h>
#include <vital/io/track_set_io.h>
#include <arrows/core/match_matrix.h>

#include <vtksys/SystemTools.hxx>

#include <vtkImageData.h>
#include <vtkImageReader2.h>
#include <vtkImageReader2Collection.h>
#include <vtkImageReader2Factory.h>
#include <vtkNew.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkXMLImageDataReader.h>

#include <qtEnumerate.h>
#include <qtIndexRange.h>
#include <qtMath.h>
#include <qtStlUtil.h>
#include <qtUiState.h>
#include <qtUiStateItem.h>

#include <QtGui/QApplication>
#include <QtGui/QColorDialog>
#include <QtGui/QDesktopServices>
#include <QtGui/QFileDialog>
#include <QtGui/QMessageBox>

#include <QtCore/QDebug>
#include <QtCore/QQueue>
#include <QtCore/QSignalMapper>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

///////////////////////////////////////////////////////////////////////////////

//BEGIN miscellaneous helpers

namespace // anonymous
{

//-----------------------------------------------------------------------------
kwiver::vital::path_t kvPath(QString const& s)
{
  return stdString(s);
}

//-----------------------------------------------------------------------------
QString cameraName(QString const& imagePath, int cameraIndex)
{
  static auto const defaultName = QString("camera%1.krtd");

  if (imagePath.isEmpty())
  {
    return defaultName.arg(cameraIndex, 4, 10, QChar('0'));
  }

  auto const fi = QFileInfo(imagePath);
  return fi.completeBaseName() + ".krtd";
}

//-----------------------------------------------------------------------------
QString findUserManual()
{
  static auto const name = "telesculptor.html";
  static auto const product = "maptk";
  static auto const version = MAPTK_VERSION;

  auto const& prefix =
    QFileInfo(QApplication::applicationFilePath()).dir().absoluteFilePath("..");

  auto locations = QStringList();

  // Install location
  locations.append(QString("%1/share/doc/%2-%3").arg(prefix, product, version));

  // Build location
  locations.append(QString("%1/doc").arg(prefix));

  foreach (auto const& path, locations)
  {
    auto const fi = QFileInfo(QString("%1/user/%2").arg(path, name));
    if (fi.exists())
    {
      // Found manual
      return fi.canonicalFilePath();
    }
  }

  // Manual not found
  return QString();
}

//-----------------------------------------------------------------------------
QSet<QString> supportedImageExtensions()
{
  QSet<QString> result;

  auto const whitespace = QRegExp("\\s");

  // Get registered readers
  vtkNew<vtkImageReader2Collection> readers;
  vtkImageReader2Factory::GetRegisteredReaders(readers.GetPointer());

  // Extract extensions for each reader
  readers->InitTraversal();
  while (auto const reader = readers->GetNextItem())
  {
    auto const extensionList =
      QString::fromLocal8Bit(reader->GetFileExtensions());
    auto const& extensions =
      extensionList.split(whitespace, QString::SkipEmptyParts);

    foreach (auto const& ext, extensions)
    {
      result.insert(ext.mid(1).toLower());
    }
  }

  return result;
}

//-----------------------------------------------------------------------------
QString makeFilters(QStringList extensions)
{
  auto result = QStringList();
  foreach (auto const& extension, extensions)
  {
    result.append("*." + extension);
  }
  return result.join(" ");
}

//-----------------------------------------------------------------------------
template <typename T>
class StateValue : public qtUiState::AbstractItem
{
public:
  StateValue(T const& defaultValue = T{}) : data{defaultValue} {}

  operator T() const { return this->data; }

  StateValue& operator=(T const& newValue)
  {
    this->data = newValue;
    return *this;
  }

  virtual QVariant value() const QTE_OVERRIDE
  {
    return QVariant::fromValue(this->data);
  }

  virtual void setValue(QVariant const& newValue) QTE_OVERRIDE
  {
    this->data = newValue.value<T>();
  }

protected:
  T data;
};

} // namespace <anonymous>

//END miscellaneous helpers

///////////////////////////////////////////////////////////////////////////////

//BEGIN MainWindowPrivate

//-----------------------------------------------------------------------------
class MainWindowPrivate
{
public:
  // Data structures
  struct CameraData
  {
    int id;
    vtkSmartPointer<vtkMaptkCamera> camera;

    QString imagePath; // Full path to camera image data
    QString depthMapPath; // Full path to depth map data
  };

  // Methods
  MainWindowPrivate()
    : activeTool(0)
    , toolUpdateActiveFrame(-1)
    , activeCameraIndex(-1) {}

  void addTool(AbstractTool* tool, MainWindow* mainWindow);

  void addCamera(kwiver::vital::camera_sptr const& camera);
  void addImage(QString const& imagePath);

  void addFrame(kwiver::vital::camera_sptr const& camera,
                QString const& imagePath);

  std::vector<std::string> imagePaths() const;
  kwiver::vital::camera_map_sptr cameraMap() const;
  void updateCameras(kwiver::vital::camera_map_sptr const&);

  void setActiveCamera(int);
  void updateCameraView();

  void loadImage(QString const& path, vtkMaptkCamera* camera);

  void loadDepthMap(QString const& imagePath);

  void setActiveTool(AbstractTool* tool);

  // Member variables
  Ui::MainWindow UI;
  Am::MainWindow AM;
  qtUiState uiState;

  StateValue<QColor>* viewBackgroundColor;

  QTimer slideTimer;
  QSignalMapper toolDispatcher;

  QAction* toolSeparator;
  AbstractTool* activeTool;
  QList<AbstractTool*> tools;
  int toolUpdateActiveFrame;
  kwiver::vital::camera_map_sptr toolUpdateCameras;
  kwiver::vital::landmark_map_sptr toolUpdateLandmarks;
  kwiver::vital::feature_track_set_sptr toolUpdateTracks;

  QList<CameraData> cameras;
  kwiver::vital::feature_track_set_sptr tracks;
  kwiver::vital::landmark_map_sptr landmarks;

  int activeCameraIndex;

  QQueue<int> orphanImages;
  QQueue<int> orphanCameras;

  vtkNew<vtkXMLImageDataReader> depthReader;
  vtkNew<vtkMaptkImageUnprojectDepth> depthFilter;
  vtkNew<vtkMaptkImageDataGeometryFilter> depthGeometryFilter;
};

QTE_IMPLEMENT_D_FUNC(MainWindow)

//-----------------------------------------------------------------------------
void MainWindowPrivate::addTool(AbstractTool* tool, MainWindow* mainWindow)
{
  this->UI.menuCompute->insertAction(this->toolSeparator, tool);

  this->toolDispatcher.setMapping(tool, tool);

  QObject::connect(tool, SIGNAL(triggered()),
                   &this->toolDispatcher, SLOT(map()));
  QObject::connect(tool, SIGNAL(updated(std::shared_ptr<ToolData>)),
                   mainWindow, SLOT(acceptToolResults(std::shared_ptr<ToolData>)));
  QObject::connect(tool, SIGNAL(completed()),
                   mainWindow, SLOT(acceptToolFinalResults()));

  this->tools.append(tool);
}

//-----------------------------------------------------------------------------
void MainWindowPrivate::addCamera(kwiver::vital::camera_sptr const& camera)
{
  if (this->orphanImages.isEmpty())
  {
    this->orphanCameras.enqueue(this->cameras.count());
    this->addFrame(camera, QString());
    return;
  }

  auto& cd = this->cameras[this->orphanImages.dequeue()];

  cd.camera = vtkSmartPointer<vtkMaptkCamera>::New();
  cd.camera->SetCamera(camera);
  cd.camera->Update();

  this->UI.worldView->addCamera(cd.id, cd.camera);
  if (cd.id == this->activeCameraIndex)
  {
    this->UI.worldView->setActiveCamera(cd.id);
    this->updateCameraView();
  }
}

//-----------------------------------------------------------------------------
void MainWindowPrivate::addImage(QString const& imagePath)
{
  if (this->orphanCameras.isEmpty())
  {
    this->orphanImages.enqueue(this->cameras.count());
    this->addFrame(kwiver::vital::camera_sptr(), imagePath);
    return;
  }

  auto& cd = this->cameras[this->orphanCameras.dequeue()];

  cd.imagePath = imagePath;

  if (cd.id == this->activeCameraIndex)
  {
    this->updateCameraView();
  }
}

//-----------------------------------------------------------------------------
void MainWindowPrivate::addFrame(
  kwiver::vital::camera_sptr const& camera, QString const& imagePath)
{
  CameraData cd;

  cd.id = this->cameras.count();

  cd.imagePath = imagePath;

  if (camera)
  {
    this->orphanImages.clear();

    cd.camera = vtkSmartPointer<vtkMaptkCamera>::New();
    cd.camera->SetCamera(camera);
    cd.camera->Update();

    this->UI.worldView->addCamera(cd.id, cd.camera);
    this->UI.actionExportCameras->setEnabled(true);
  }
  else
  {
    this->orphanCameras.clear();
  }

  this->cameras.append(cd);

  this->UI.camera->setRange(0, this->cameras.count() - 1);
  this->UI.cameraSpin->setRange(0, this->cameras.count() - 1);

  // When the first camera is added, show it immediately and reset the camera
  // view, and enable slideshow controls
  if (this->cameras.count() == 1)
  {
    this->UI.actionSlideshowPlay->setEnabled(true);
    this->UI.camera->setEnabled(true);
    this->UI.cameraSpin->setEnabled(true);

    this->setActiveCamera(0);
    this->UI.cameraView->resetView();
  }
}

//-----------------------------------------------------------------------------
std::vector<std::string> MainWindowPrivate::imagePaths() const
{
  std::vector<std::string> paths(this->cameras.count());

  foreach (auto i, qtIndexRange(this->cameras.count()))
  {
    auto const& cd = this->cameras[i];
    paths[i] = cd.imagePath.toStdString();
  }

  return paths;
}

//-----------------------------------------------------------------------------
kwiver::vital::camera_map_sptr MainWindowPrivate::cameraMap() const
{
  kwiver::vital::camera_map::map_camera_t map;

  foreach (auto i, qtIndexRange(this->cameras.count()))
  {
    auto const& cd = this->cameras[i];
    if (cd.camera)
    {
      map.insert(std::make_pair(static_cast<kwiver::vital::frame_id_t>(i),
                                cd.camera->GetCamera()));
    }
  }

  return std::make_shared<kwiver::vital::simple_camera_map>(map);
}

//-----------------------------------------------------------------------------
void MainWindowPrivate::updateCameras(
  kwiver::vital::camera_map_sptr const& cameras)
{
  auto const cameraCount = this->cameras.count();
  auto allowExport = false;

  foreach (auto const& iter, cameras->cameras())
  {
    auto const index = static_cast<int>(iter.first);
    if (index >= 0 && index < cameraCount && iter.second)
    {
      auto& cd = this->cameras[index];
      if (!cd.camera)
      {
        cd.camera = vtkSmartPointer<vtkMaptkCamera>::New();
        this->UI.worldView->addCamera(cd.id, cd.camera);
      }
      cd.camera->SetCamera(iter.second);
      cd.camera->Update();

      if (cd.id == this->activeCameraIndex)
      {
        this->UI.worldView->setActiveCamera(cd.id);
        this->updateCameraView();
      }

      allowExport = allowExport || iter.second;
    }
  }

  this->UI.actionExportCameras->setEnabled(allowExport);
}

//-----------------------------------------------------------------------------
void MainWindowPrivate::setActiveCamera(int id)
{
  this->activeCameraIndex = id;
  this->UI.worldView->setActiveCamera(id);
  this->updateCameraView();

  auto& cd = this->cameras[id];
  if (!cd.depthMapPath.isEmpty())
  {
    this->loadDepthMap(cd.depthMapPath);
  }

  UI.worldView->setVolumeCurrentFramePath(cd.imagePath);
}

//-----------------------------------------------------------------------------
void MainWindowPrivate::updateCameraView()
{
  if (this->activeCameraIndex < 0)
  {
    this->loadImage(QString(), 0);
    this->UI.cameraView->setActiveFrame(static_cast<unsigned>(-1));
    this->UI.cameraView->clearLandmarks();
    return;
  }

  this->UI.cameraView->setActiveFrame(
    static_cast<unsigned>(this->activeCameraIndex));

  QHash<kwiver::vital::track_id_t, kwiver::vital::vector_2d> landmarkPoints;

  auto const& cd = this->cameras[this->activeCameraIndex];

  // Show camera image
  this->loadImage(cd.imagePath, cd.camera);
  this->UI.cameraView->setImagePath(cd.imagePath);

  if (!cd.camera)
  {
    // Can't show landmarks or residuals with no camera
    this->UI.cameraView->clearLandmarks();
    this->UI.cameraView->clearResiduals();
    return;
  }

  // Show landmarks
  this->UI.cameraView->clearLandmarks();
  if (this->landmarks)
  {
    // Map landmarks to camera space
    auto const& landmarks = this->landmarks->landmarks();
    foreach (auto const& lm, landmarks)
    {
      double pp[2];
      if (cd.camera->ProjectPoint(lm.second->loc(), pp))
      {
        // Add projected landmark to camera view
        auto const id = lm.first;
        this->UI.cameraView->addLandmark(id, pp[0], pp[1]);
        landmarkPoints.insert(id, kwiver::vital::vector_2d(pp[0], pp[1]));
      }
    }
  }

  // Show residuals
  this->UI.cameraView->clearResiduals();
  if (this->tracks)
  {
    auto const& tracks = this->tracks->tracks();
    foreach (auto const& track, tracks)
    {
      auto const& state = track->find(this->activeCameraIndex);
      if ( state == track->end() )
      {
        continue;
      }
      auto fts = std::dynamic_pointer_cast<kwiver::vital::feature_track_state>(*state);
      if ( fts && fts->feature)
      {
        auto const id = track->id();
        if (landmarkPoints.contains(id))
        {
          auto const& fp = fts->feature->loc();
          auto const& lp = landmarkPoints[id];
          this->UI.cameraView->addResidual(id, fp[0], fp[1], lp[0], lp[1]);
        }
      }
    }
  }
}

//-----------------------------------------------------------------------------
void MainWindowPrivate::loadImage(QString const& path, vtkMaptkCamera* camera)
{
  if (path.isEmpty())
  {
    auto imageDimensions = QSize(1, 1);
    if (camera)
    {
      int w, h;
      camera->GetImageDimensions(w, h);
      imageDimensions = QSize(w, h);
    }

    this->UI.cameraView->setImageData(0, imageDimensions);
    this->UI.worldView->setImageData(0, imageDimensions);
  }
  else
  {
    // Create a reader capable of reading the image file
    auto const reader =
      vtkImageReader2Factory::CreateImageReader2(qPrintable(path));
    if (!reader)
    {
      qWarning() << "Failed to create image reader for image" << path;
      this->loadImage(QString(), camera);
      return;
    }

    // Load the image
    reader->SetFileName(qPrintable(path));
    reader->Update();

    // Get dimensions
    auto const data = reader->GetOutput();
    int dimensions[3];
    data->GetDimensions(dimensions);

    // Test for errors
    if (dimensions[0] < 2 || dimensions[1] < 2)
    {
      qWarning() << "Failed to read image" << path;
      this->loadImage(QString(), camera);
    }
    else
    {
      // If successful, update camera image dimensions
      if (camera)
      {
        camera->SetImageDimensions(dimensions);
      }

      // Set image on views
      auto const size = QSize(dimensions[0], dimensions[1]);
      this->UI.cameraView->setImageData(data, size);
      this->UI.worldView->setImageData(data, size);
    }

    // Delete the reader
    reader->Delete();
  }
}


//-----------------------------------------------------------------------------
void MainWindowPrivate::loadDepthMap(QString const& imagePath)
{
  if (!vtksys::SystemTools::FileExists(qPrintable(imagePath), true))
  {
    qWarning() << "File doesn't exist: " << imagePath;
    return;
  }

  if (this->depthReader->GetFileName() &&
    !strcmp(this->depthReader->GetFileName(), qPrintable(imagePath)))
  {
    // No change to reader input... return without any update
    return;
  }

  this->depthReader->SetFileName(qPrintable(imagePath));

  this->UI.depthMapView->setValidDepthInput(true);
  this->UI.worldView->setValidDepthInput(true);

  this->depthFilter->SetCamera(this->cameras[this->activeCameraIndex].camera);
  this->UI.worldView->updateDepthMap();
  this->UI.depthMapView->updateView(true);
  this->UI.actionExportDepthPoints->setEnabled(true);
}

//-----------------------------------------------------------------------------
void MainWindowPrivate::setActiveTool(AbstractTool* tool)
{
  // Disconnect cancel action
  QObject::disconnect(this->UI.actionCancelComputation, 0, this->activeTool, 0);

  // Update current tool
  this->activeTool = tool;

  // Connect cancel action
  if (tool)
  {
    QObject::connect(this->UI.actionCancelComputation, SIGNAL(triggered()),
                     tool, SLOT(cancel()));
  }

  auto const enableTools = !tool;
  auto const enableCancel = tool && tool->isCancelable();
  foreach (auto const& tool, this->tools)
  {
    tool->setEnabled(enableTools);
  }
  this->UI.actionCancelComputation->setEnabled(enableCancel);
  this->UI.actionOpen->setEnabled(enableTools);
}

//END MainWindowPrivate

///////////////////////////////////////////////////////////////////////////////

//BEGIN MainWindow

//-----------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent, Qt::WindowFlags flags)
  : QMainWindow(parent, flags), d_ptr(new MainWindowPrivate)
{
  QTE_D();

  // Set up UI
  d->UI.setupUi(this);
  d->AM.setupActions(d->UI, this);

  d->toolSeparator =
    d->UI.menuCompute->insertSeparator(d->UI.actionCancelComputation);

  d->addTool(new TrackFeaturesTool(this), this);
  d->addTool(new InitCamerasLandmarksTool(this), this);
  d->addTool(new BundleAdjustTool(this), this);
  d->addTool(new CanonicalTransformTool(this), this);
  d->addTool(new NeckerReversalTool(this), this);
  d->addTool(new TrackFilterTool(this), this);

  d->UI.menuView->addSeparator();
  d->UI.menuView->addAction(d->UI.cameraViewDock->toggleViewAction());
  d->UI.menuView->addAction(d->UI.cameraSelectorDock->toggleViewAction());
  d->UI.menuView->addAction(d->UI.depthMapViewDock->toggleViewAction());

  d->UI.playSlideshowButton->setDefaultAction(d->UI.actionSlideshowPlay);
  d->UI.loopSlideshowButton->setDefaultAction(d->UI.actionSlideshowLoop);

  connect(d->UI.actionOpen, SIGNAL(triggered()), this, SLOT(openFile()));
  connect(d->UI.actionQuit, SIGNAL(triggered()), qApp, SLOT(quit()));

  connect(d->UI.actionShowWorldAxes, SIGNAL(toggled(bool)),
          d->UI.worldView, SLOT(setAxesVisible(bool)));

  connect(d->UI.actionExportCameras, SIGNAL(triggered()),
          this, SLOT(saveCameras()));
  connect(d->UI.actionExportLandmarks, SIGNAL(triggered()),
          this, SLOT(saveLandmarks()));
  connect(d->UI.actionExportVolume, SIGNAL(triggered()),
          this, SLOT(saveVolume()));
  connect(d->UI.actionExportMesh, SIGNAL(triggered()),
          this, SLOT(saveMesh()));
  connect(d->UI.actionExportColoredMesh, SIGNAL(triggered()),
          this, SLOT(saveColoredMesh()));
  connect(d->UI.actionExportDepthPoints, SIGNAL(triggered()),
          this, SLOT(saveDepthPoints()));
  connect(d->UI.actionExportTracks, SIGNAL(triggered()),
          this, SLOT(saveTracks()));

  connect(d->UI.worldView, SIGNAL(depthMapEnabled(bool)),
          this, SLOT(enableSaveDepthPoints(bool)));

  connect(d->UI.actionShowMatchMatrix, SIGNAL(triggered()),
          this, SLOT(showMatchMatrix()));

  connect(&d->toolDispatcher, SIGNAL(mapped(QObject*)),
          this, SLOT(executeTool(QObject*)));

  connect(d->UI.actionSetBackgroundColor, SIGNAL(triggered()),
          this, SLOT(setViewBackroundColor()));

  connect(d->UI.actionAbout, SIGNAL(triggered()),
          this, SLOT(showAboutDialog()));
  connect(d->UI.actionShowManual, SIGNAL(triggered()),
          this, SLOT(showUserManual()));

  connect(&d->slideTimer, SIGNAL(timeout()), this, SLOT(nextSlide()));
  connect(d->UI.actionSlideshowPlay, SIGNAL(toggled(bool)),
          this, SLOT(setSlideshowPlaying(bool)));
  connect(d->UI.slideDelay, SIGNAL(valueChanged(int)),
          this, SLOT(setSlideDelay(int)));

  connect(d->UI.camera, SIGNAL(valueChanged(int)),
          this, SLOT(setActiveCamera(int)));

  connect(d->UI.worldView, SIGNAL(meshEnabled(bool)),
          this, SLOT(enableSaveMesh(bool)));

  connect(d->UI.worldView, SIGNAL(coloredMeshEnabled(bool)),
          this, SLOT(enableSaveColoredMesh(bool)));

  connect(d->UI.worldView, SIGNAL(depthMapThresholdsChanged()),
          d->UI.depthMapView, SLOT(updateThresholds()));

  connect(d->UI.depthMapViewDock, SIGNAL(visibilityChanged(bool)),
          d->UI.depthMapView, SLOT(updateView(bool)));

  this->setSlideDelay(d->UI.slideDelay->value());

#ifdef VTKWEBGLEXPORTER
  d->UI.actionWebGLScene->setVisible(true);
  connect(d->UI.actionWebGLScene, SIGNAL(triggered(bool)),
          this, SLOT(saveWebGLScene()));
#endif

  // Set up UI persistence and restore previous state
  auto const sdItem = new qtUiState::Item<int, QSlider>(
    d->UI.slideDelay, &QSlider::value, &QSlider::setValue);
  d->uiState.map("SlideDelay", sdItem);

  d->viewBackgroundColor = new StateValue<QColor>{Qt::black},
  d->uiState.map("ViewBackground", d->viewBackgroundColor);

  d->uiState.mapChecked("WorldView/Axes", d->UI.actionShowWorldAxes);

  d->uiState.mapState("Window/state", this);
  d->uiState.mapGeometry("Window/geometry", this);
  d->uiState.restore();

  d->UI.worldView->setBackgroundColor(*d->viewBackgroundColor);
  d->UI.cameraView->setBackgroundColor(*d->viewBackgroundColor);
  d->UI.depthMapView->setBackgroundColor(*d->viewBackgroundColor);

  // Hookup basic depth pipeline and pass geometry filter to relevant views
  d->depthFilter->SetInputConnection(d->depthReader->GetOutputPort());
  d->depthGeometryFilter->SetInputConnection(d->depthFilter->GetOutputPort());
  d->UI.worldView->setDepthGeometryFilter(d->depthGeometryFilter.GetPointer());
  d->UI.depthMapView->setDepthGeometryFilter(d->depthGeometryFilter.GetPointer());

  d->UI.worldView->resetView();
}

//-----------------------------------------------------------------------------
MainWindow::~MainWindow()
{
  QTE_D();
  d->uiState.save();
}

//-----------------------------------------------------------------------------
void MainWindow::openFile()
{
  static auto const imageFilters =
    makeFilters(supportedImageExtensions().toList());

  auto const paths = QFileDialog::getOpenFileNames(
    this, "Open File", QString(),
    "All Supported Files (*.conf *.txt *.ply *.krtd " + imageFilters + ");;"
    "Project configuration file (*.conf);;"
    "Track file (*.txt);;"
    "Landmark file (*.ply);;"
    "Camera file (*.krtd);;"
    "All Files (*)");

  if (!paths.isEmpty())
  {
    this->openFiles(paths);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::openFile(QString const& path)
{
  static auto const imageExtensions = supportedImageExtensions();

  auto const fi = QFileInfo(path);
  if (fi.suffix().toLower() == "conf")
  {
    this->loadProject(path);
  }
  else if (fi.suffix().toLower() == "txt")
  {
    this->loadTracks(path);
  }
  else if (fi.suffix().toLower() == "ply")
  {
    this->loadLandmarks(path);
  }
  else if (fi.suffix().toLower() == "krtd")
  {
    this->loadCamera(path);
  }
  else if (imageExtensions.contains(fi.suffix().toLower()))
  {
    this->loadImage(path);
  }
  else
  {
    qWarning() << "Don't know how to read file" << path
               << "(unrecognized extension)";
  }
}

//-----------------------------------------------------------------------------
void MainWindow::openFiles(QStringList const& paths)
{
  foreach (auto const& path, paths)
  {
    this->openFile(path);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::loadProject(QString const& path)
{
  QTE_D();

  Project project;
  if (!project.read(path))
  {
    qWarning() << "Failed to load project from" << path; // TODO dialog?
    return;
  }

  // Load tracks
  if (!project.tracks.isEmpty())
  {
    this->loadTracks(project.tracks);
  }

  // Load landmarks
  if (!project.landmarks.isEmpty())
  {
    this->loadLandmarks(project.landmarks);
  }

  // Load cameras and/or images
  if (project.cameraPath.isEmpty())
  {
    foreach (auto const& ip, project.images)
    {
      d->addImage(ip);
    }
  }
  else
  {
    auto const cameraDir = kwiver::vital::path_t(kvPath(project.cameraPath));
    foreach (auto const& ip, project.images)
    {
      try
      {
        auto const& camera =
          kwiver::vital::read_krtd_file(kvPath(ip), cameraDir);

        // Add camera to scene
        d->addFrame(camera, ip);
      }
      catch (...)
      {
        qWarning() << "failed to read camera for" << ip
                   << "from" << project.cameraPath;
        d->addFrame(kwiver::vital::camera_sptr(), ip);
      }
    }
  }

  // Associate depth maps with cameras
  foreach (auto dm, qtEnumerate(project.depthMaps))
  {
    auto const i = dm.key();
    if (i >= 0 && i < d->cameras.count())
    {
      d->cameras[i].depthMapPath = dm.value();
    }

    if (i == d->activeCameraIndex)
    {
      d->loadDepthMap(dm.value());
    }
  }

#ifdef VTKWEBGLEXPORTER
  d->UI.actionWebGLScene->setEnabled(true);
#endif

  //Load volume
  if (!project.volumePath.isEmpty())
  {

    d->UI.worldView->loadVolume(project.volumePath,d->cameras.size(),
                                project.cameraPath, project.imageListPath);
  }

  d->UI.worldView->resetView();
}

//-----------------------------------------------------------------------------
void MainWindow::loadImage(QString const& path)
{
  QTE_D();
  d->addImage(path);
}

//-----------------------------------------------------------------------------
void MainWindow::loadCamera(QString const& path)
{
  QTE_D();

  try
  {
    auto const& camera = kwiver::vital::read_krtd_file(kvPath(path));
    d->addCamera(camera);
  }
  catch (...)
  {
    qWarning() << "failed to read camera from" << path;
  }
}

//-----------------------------------------------------------------------------
void MainWindow::loadTracks(QString const& path)
{
  QTE_D();

  try
  {
    auto const& tracks = kwiver::vital::read_feature_track_file(kvPath(path));
    if (tracks)
    {
      d->tracks = tracks;
      d->updateCameraView();

      foreach (auto const& track, tracks->tracks())
      {
        d->UI.cameraView->addFeatureTrack(*track);
      }

      d->UI.actionExportTracks->setEnabled(
          d->tracks && d->tracks->size());

      d->UI.actionShowMatchMatrix->setEnabled(!tracks->tracks().empty());
    }
  }
  catch (...)
  {
    qWarning() << "failed to read tracks from" << path;
  }
}

//-----------------------------------------------------------------------------
void MainWindow::loadLandmarks(QString const& path)
{
  QTE_D();

  try
  {
    auto const& landmarks = kwiver::vital::read_ply_file(kvPath(path));
    if (landmarks)
    {
      d->landmarks = landmarks;
      d->UI.worldView->setLandmarks(*landmarks);
      d->UI.cameraView->setLandmarksData(*landmarks);

      d->UI.actionExportLandmarks->setEnabled(
        d->landmarks && d->landmarks->size());

      d->updateCameraView();
    }
  }
  catch (...)
  {
    qWarning() << "failed to read landmarks from" << path;
  }
}

//-----------------------------------------------------------------------------
void MainWindow::saveLandmarks()
{
  auto const path = QFileDialog::getSaveFileName(
    this, "Export Landmarks", QString(),
    "Landmark file (*.ply);;"
    "All Files (*)");

  if (!path.isEmpty())
  {
    this->saveLandmarks(path);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::saveLandmarks(QString const& path)
{
  QTE_D();

  try
  {
    kwiver::vital::write_ply_file(d->landmarks, kvPath(path));
  }
  catch (...)
  {
    auto const msg =
      QString("An error occurred while exporting landmarks to \"%1\". "
              "The output file may not have been written correctly.");
    QMessageBox::critical(this, "Export error", msg.arg(path));
  }
}

//-----------------------------------------------------------------------------
void MainWindow::saveTracks()
{
  auto const path = QFileDialog::getSaveFileName(
    this, "Export Tracks", QString(),
    "Track file (*.txt);;"
    "All Files (*)");

  if (!path.isEmpty())
  {
    this->saveTracks(path);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::saveTracks(QString const& path)
{
  QTE_D();

  try
  {
    kwiver::vital::write_feature_track_file(d->tracks, kvPath(path));
  }
  catch (...)
  {
    auto const msg =
      QString("An error occurred while exporting tracks to \"%1\". "
              "The output file may not have been written correctly.");
    QMessageBox::critical(this, "Export error", msg.arg(path));
  }
}

//-----------------------------------------------------------------------------
void MainWindow::saveCameras()
{
  auto const path = QFileDialog::getExistingDirectory(this, "Export Cameras");

  if (!path.isEmpty())
  {
    this->saveCameras(path);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::saveCameras(QString const& path)
{
  QTE_D();

  auto out = QHash<QString, kwiver::vital::camera_sptr>();
  auto willOverwrite = QStringList();

  foreach (auto i, qtIndexRange(d->cameras.count()))
  {
    auto const& cd = d->cameras[i];
    if (cd.camera)
    {
      auto const camera = cd.camera->GetCamera();
      if (camera)
      {
        auto const filepath = path + "/" + cameraName(cd.imagePath, i);
        out.insert(filepath, camera);

        if (QFileInfo(filepath).exists())
        {
          willOverwrite.append(filepath);
        }
      }
    }
  }

  if (!willOverwrite.isEmpty())
  {
    QMessageBox mb(QMessageBox::Warning, "Confirm overwrite",
                   "One or more files will be overwritten by this operation. "
                   "Do you wish to continue?", QMessageBox::Cancel, this);

    mb.addButton("&Overwrite", QMessageBox::AcceptRole);
    mb.setDetailedText("The following file(s) will be overwritten:\n  " +
                       willOverwrite.join("  \n"));

    if (mb.exec() != QDialog::Accepted)
    {
      // User canceled operation
      return;
    }
  }

  auto errors = QStringList();
  foreach (auto const& iter, qtEnumerate(out))
  {
    try
    {
      kwiver::vital::write_krtd_file(*iter.value(), kvPath(iter.key()));
    }
    catch (...)
    {
      errors.append(iter.key());
    }
  }

  if (!errors.isEmpty())
  {
    auto const msg =
      QString("Error(s) occurred while exporting cameras to \"%1\". "
              "One or more output files may not have been written correctly.");

    QMessageBox mb(QMessageBox::Critical, "Export error", msg.arg(path),
                   QMessageBox::Ok, this);

    mb.setDetailedText("Error writing the following file(s):\n  " +
                       errors.join("  \n"));

    mb.exec();
  }
}

//-----------------------------------------------------------------------------
void MainWindow::enableSaveDepthPoints(bool state)
{
  QTE_D();

  if (state && d->depthGeometryFilter->GetOutput()->GetNumberOfVerts() <= 0)
  {
    state = false;
  }
  d->UI.actionExportDepthPoints->setEnabled(state);
}

//-----------------------------------------------------------------------------
void MainWindow::saveDepthPoints()
{
  auto const path = QFileDialog::getSaveFileName(
    this, "Export Depth Point Cloud", QString(),
    "PLY file (*.ply);;"
    "All Files (*)");

  if (!path.isEmpty())
  {
    this->saveDepthPoints(path);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::saveDepthPoints(QString const& path)
{
  QTE_D();

  try
  {
    d->UI.worldView->saveDepthPoints(path);
  }
  catch (...)
  {
    auto const msg =
      QString("An error occurred while exporting depth points to \"%1\". "
              "The output file may not have been written correctly.");
    QMessageBox::critical(this, "Export error", msg.arg(path));
  }
}

//-----------------------------------------------------------------------------
void MainWindow::saveWebGLScene()
{
#ifdef VTKWEBGLEXPORTER
  QTE_D();

  auto const path = QFileDialog::getSaveFileName(
    this, "Export Scene to WebGL", QString(),
    "WebGL scene file (*.html);;"
    "All Files (*)");

  if (!path.isEmpty())
  {
    d->UI.worldView->exportWebGLScene(path);
  }
#endif
}

//-----------------------------------------------------------------------------
void MainWindow::enableSaveMesh(bool state)
{
  QTE_D();

  d->UI.actionExportVolume->setEnabled(state);
  d->UI.actionExportMesh->setEnabled(state);
}

//-----------------------------------------------------------------------------
void MainWindow::enableSaveColoredMesh(bool state)
{
  QTE_D();

  d->UI.actionExportColoredMesh->setEnabled(state);
}

//-----------------------------------------------------------------------------
void MainWindow::saveMesh()
{
  QTE_D();

  auto const path = QFileDialog::getSaveFileName(
    this, "Export Mesh", QString("mesh.vtp"),
    "Mesh file (*.vtp);;"
    "All Files (*)");

  if (!path.isEmpty())
  {
    d->UI.worldView->saveMesh(path);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::saveVolume()
{
  QTE_D();

  auto const path = QFileDialog::getSaveFileName(
    this, "Export Volume", QString("volume.vts"),
    "Mesh file (*.vts);;"
    "All Files (*)");

  if (!path.isEmpty())
  {
    d->UI.worldView->saveVolume(path);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::saveColoredMesh()
{
  QTE_D();

  auto const path = QFileDialog::getSaveFileName(
    this, "Export Colored Mesh", QString("colored_mesh.vtp"),
    "VTK Polydata (*.vtp);;"
    "PLY File (*.ply);;"
    "All Files (*)");

  if (!path.isEmpty())
  {
    d->UI.worldView->saveColoredMesh(path);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::setSlideDelay(int delayExp)
{
  QTE_D();

  static auto const ttFormat =
    QString("%1 (%2)").arg(d->UI.slideDelay->toolTip());

  auto const de = static_cast<double>(delayExp) * 0.1;
  auto const delay = qRound(qPow(10.0, de));
  d->slideTimer.setInterval(delay);

  if (delay < 1000)
  {
    auto const fps = 1e3 / delay;
    auto const dt = QString("%1 / sec").arg(fps, 0, 'f', 1);
    d->UI.slideDelay->setToolTip(ttFormat.arg(dt));
  }
  else
  {
    auto const dt = QString("%1 sec").arg(delay / 1e3, 0, 'f', 1);
    d->UI.slideDelay->setToolTip(ttFormat.arg(dt));
  }
}

//-----------------------------------------------------------------------------
void MainWindow::setSlideshowPlaying(bool playing)
{
  QTE_D();
  if (playing)
  {
    if (d->UI.camera->value() == d->UI.camera->maximum())
    {
      d->UI.camera->triggerAction(QAbstractSlider::SliderToMinimum);
    }
    d->slideTimer.start();
  }
  else
  {
    d->slideTimer.stop();
  }

  d->UI.camera->setEnabled(!playing);
}

//-----------------------------------------------------------------------------
void MainWindow::nextSlide()
{
  QTE_D();

  if (d->UI.camera->value() == d->UI.camera->maximum())
  {
    if (d->UI.actionSlideshowLoop->isChecked())
    {
      d->UI.camera->triggerAction(QAbstractSlider::SliderToMinimum);
    }
    else
    {
      d->UI.actionSlideshowPlay->setChecked(false);
    }
  }
  else
  {
    d->UI.camera->triggerAction(QAbstractSlider::SliderSingleStepAdd);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::setActiveCamera(int id)
{
  QTE_D();

  if (id < 0 || id >= d->cameras.count())
  {
    qDebug() << "MainWindow::setActiveCamera:"
             << " requested ID" << id << "is invalid";
    return;
  }

  d->setActiveCamera(id);
}

//-----------------------------------------------------------------------------
void MainWindow::executeTool(QObject* object)
{
  QTE_D();

  auto const tool = qobject_cast<AbstractTool*>(object);
  if (tool && !d->activeTool)
  {
    d->setActiveTool(tool);
    tool->setActiveFrame(d->activeCameraIndex);
    tool->setImagePaths(d->imagePaths());
    tool->setTracks(d->tracks);
    tool->setCameras(d->cameraMap());
    tool->setLandmarks(d->landmarks);

    if (!tool->execute())
    {
      d->setActiveTool(0);
    }
  }
}

//-----------------------------------------------------------------------------
void MainWindow::acceptToolFinalResults()
{
  QTE_D();
  if (d->activeTool)
  {
    acceptToolResults(d->activeTool->data());
  }
  d->setActiveTool(0);
}

//-----------------------------------------------------------------------------
void MainWindow::acceptToolResults(std::shared_ptr<ToolData> data)
{
  QTE_D();
  // if all the update variables are Null then trigger a GUI update after
  // extracting the data otherwise we've already triggered an update that
  // hasn't happened yet, so don't trigger another
  bool updateNeeded = !d->toolUpdateCameras &&
                      !d->toolUpdateLandmarks &&
                      !d->toolUpdateTracks &&
                      d->toolUpdateActiveFrame < 0;

  if (d->activeTool)
  {
    auto const outputs = d->activeTool->outputs();

    d->toolUpdateCameras = NULL;
    d->toolUpdateLandmarks = NULL;
    d->toolUpdateTracks = NULL;
    d->toolUpdateActiveFrame = -1;
    if (outputs.testFlag(AbstractTool::Cameras))
    {
      d->toolUpdateCameras = data->cameras;
    }
    if (outputs.testFlag(AbstractTool::Landmarks))
    {
      d->toolUpdateLandmarks = data->landmarks;
    }
    if (outputs.testFlag(AbstractTool::Tracks))
    {
      d->toolUpdateTracks = data->tracks;
    }
    if (outputs.testFlag(AbstractTool::ActiveFrame))
    {
      d->toolUpdateActiveFrame = static_cast<int>(data->activeFrame);
    }
  }

  if(updateNeeded)
  {
    QTimer::singleShot(1000, this, SLOT(updateToolResults()));
  }
}

//-----------------------------------------------------------------------------
void MainWindow::updateToolResults()
{
  QTE_D();

  if (d->toolUpdateCameras)
  {
    d->updateCameras(d->toolUpdateCameras);
    d->toolUpdateCameras = NULL;
  }
  if (d->toolUpdateLandmarks)
  {
    d->landmarks = d->toolUpdateLandmarks;
    d->UI.worldView->setLandmarks(*d->landmarks);

    d->UI.actionExportLandmarks->setEnabled(
      d->landmarks && d->landmarks->size());
    d->toolUpdateLandmarks = NULL;
  }
  if (d->toolUpdateTracks)
  {
    d->tracks = d->toolUpdateTracks;
    d->UI.cameraView->clearFeatureTracks();
    d->updateCameraView();

    foreach (auto const& track, d->tracks->tracks())
    {
      d->UI.cameraView->addFeatureTrack(*track);
    }
    d->UI.actionExportTracks->setEnabled(
        d->tracks && d->tracks->size());

    d->UI.actionShowMatchMatrix->setEnabled(!d->tracks->tracks().empty());
    d->toolUpdateTracks = NULL;
  }
  if (d->toolUpdateActiveFrame >= 0)
  {
    d->UI.camera->setValue(d->toolUpdateActiveFrame);
    this->setActiveCamera(d->toolUpdateActiveFrame);
    d->toolUpdateActiveFrame = -1;
  }

  if (!d->cameras.isEmpty())
  {
    d->setActiveCamera(d->activeCameraIndex);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::showMatchMatrix()
{
  QTE_D();

  if (d->tracks)
  {
    // Get matrix
    auto frames = std::vector<kwiver::vital::frame_id_t>();
    auto const mm = kwiver::arrows::match_matrix(d->tracks, frames);

    // Show window
    auto window = new MatchMatrixWindow();
    window->setMatrix(mm, frames);
    window->show();
  }
}

//-----------------------------------------------------------------------------
void MainWindow::setViewBackroundColor()
{
  QTE_D();

  QColorDialog dlg;
  dlg.setCurrentColor(*d->viewBackgroundColor);
  if (dlg.exec() == QDialog::Accepted)
  {
    *d->viewBackgroundColor = dlg.currentColor();
    d->UI.worldView->setBackgroundColor(*d->viewBackgroundColor);
    d->UI.cameraView->setBackgroundColor(*d->viewBackgroundColor);
    d->UI.depthMapView->setBackgroundColor(*d->viewBackgroundColor);
  }
}

//-----------------------------------------------------------------------------
void MainWindow::showAboutDialog()
{
  AboutDialog dlg(this);
  dlg.exec();
}

//-----------------------------------------------------------------------------
void MainWindow::showUserManual()
{
  auto const path = findUserManual();
  if (!path.isEmpty())
  {
    auto const& uri = QUrl::fromLocalFile(path);
    QDesktopServices::openUrl(uri);
  }
  else
  {
    QMessageBox::information(
      this, "Not found",
      "The user manual could not be located. Please check your installation.");
  }
}

//END MainWindow
