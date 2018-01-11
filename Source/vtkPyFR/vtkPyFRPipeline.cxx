#include <algorithm>
#include <sstream>
#include <mpi.h>

#include "vtkPyFRPipeline.h"

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCollection.h>
#include <vtkCPDataDescription.h>
#include <vtkCPInputDataDescription.h>
#include <vtkDataSetMapper.h>
#include <vtkLiveInsituLink.h>
#include <vtkMPICommunicator.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPlane.h>
#include <vtkPolyDataMapper.h>
#include <vtkPVLiveRenderView.h>
#include <vtkPVTrivialProducer.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkRenderWindow.h>
#include <vtkSmartPointer.h>
#include <vtkSMDoubleVectorProperty.h>
#include <vtkSMInputProperty.h>
#include <vtkSMIntVectorProperty.h>
#include <vtkSMOutputPort.h>
#include <vtkSMParaViewPipelineControllerWithRendering.h>
#include <vtkSMPluginManager.h>
#include <vtkSMPropertyHelper.h>
#include <vtkSMProxyListDomain.h>
#include <vtkSMProxyManager.h>
#include <vtkSMPVRepresentationProxy.h>
#include <vtkSMRenderViewProxy.h>
#include <vtkSMRepresentationProxy.h>
#include <vtkSMSessionProxyManager.h>
#include <vtkSMSourceProxy.h>
#include <vtkSMStringVectorProperty.h>
#include <vtkSMTransferFunctionManager.h>
#include <vtkSMTransferFunctionProxy.h>
#include <vtkSMViewProxy.h>
#include <vtkSMWriterProxy.h>
#include <vtkSTLReader.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>
#include <vtkTextWidget.h>
#include <vtkTextRepresentation.h>
#include <vtkVector.h>
#include <vtkXMLUnstructuredGridReader.h>
#include <vtkProperty.h>

#include "vtkPyFRData.h"
#include "vtkPyFRCrinkleClipFilter.h"
#include "vtkPyFRContourData.h"
#include "vtkPyFRContourFilter.h"
#include "vtkPyFRGradientFilter.h"
#include "vtkPyFRMapper.h"
#include "vtkPyFRMergePointsFilter.h"
#include "vtkPyFRParallelSliceFilter.h"
#include "vtkXMLPyFRDataWriter.h"
#include "vtkXMLPyFRContourDataWriter.h"
#include "vtkPVPlugin.h"

#include "PyFRData.h"

#define STRINGIFY(s) TOSTRING(s)
#define TOSTRING(s) #s

#ifdef PYFR_SINGLE
PV_PLUGIN_IMPORT_INIT(pyfr_plugin_fp32)
#else
PV_PLUGIN_IMPORT_INIT(pyfr_plugin_fp64)
#endif

#undef root
#define root(stmt) \
  do { \
    vtkMPICommunicator* comm = vtkMPICommunicator::GetWorldCommunicator(); \
    const vtkIdType rank = comm->GetLocalProcessId(); \
    if(0 == rank) { \
      stmt; \
    } \
  } while(0)

namespace {
// Simplified reduction interface that assumes we want to go to the root.
template<typename T> void
reduce(T* v, size_t n, vtkCommunicator::StandardOperations op) {
  vtkMPICommunicator* comm = vtkMPICommunicator::GetWorldCommunicator();
  const vtkIdType rank = comm->GetLocalProcessId();
  const int dest = 0;
  if(rank == 0) {
    comm->Reduce((T*)MPI_IN_PLACE, v, n, op, dest);
  } else {
    comm->Reduce(v, NULL, n, op, dest);
  }
}


static void
output_camera(/*const*/ vtkCamera* cam) {
  double eye[3] = {0.0};
  double ref[3] = {0.0};
  double vup[3] = {0.0};
  cam->GetPosition(eye);
  cam->GetFocalPoint(ref);
  cam->GetViewUp(vup);
  printf("eye={%lg %lg %lg} ref={%lf %lf %lf} vup={%lf %lf %lf}\n",
         eye[0],eye[1],eye[2], ref[0],ref[1],ref[2],
         vup[0],vup[1],vup[2]);
}

template <class Mapper>
void vtkAddActor(vtkSmartPointer<Mapper> mapper,
                 vtkSMSourceProxy* filter,
                 vtkSMViewProxy* view,
                 float yPos=0
                )
{
  vtkSMRenderViewProxy* rview = vtkSMRenderViewProxy::SafeDownCast(view);
  rview->UpdateVTKObjects();
  filter->UpdateVTKObjects();

  vtkRenderer* ren = rview->GetRenderer();
  vtkAlgorithm* algo = vtkAlgorithm::SafeDownCast(filter->GetClientSideObject());

  mapper->SetInputConnection(algo->GetOutputPort());

  vtkNew<vtkActor> actor;
  actor->SetMapper(mapper);

  actor->GetProperty()->SetSpecular(0.5);
  actor->GetProperty()->SetSpecularPower(25.0);
  printf("Displacement: %f", yPos);
  actor->SetPosition(0, yPos, 0);

  ren->AddActor(actor.GetPointer());
  
}

void vtkUpdateFilter(vtkSMSourceProxy* filter, double time)
{
  // std::cout << "filter: " << filter->GetClassName() << " update to time: " << time << std::endl;
  filter->UpdatePipeline(time);
}


}

vtkStandardNewMacro(vtkPyFRPipeline);

//----------------------------------------------------------------------------
vtkPyFRPipeline::vtkPyFRPipeline()
{
  //this->WhichPipeline = 1;
  this->InsituLink = NULL;
}

//----------------------------------------------------------------------------
vtkPyFRPipeline::~vtkPyFRPipeline()
{
  if (this->InsituLink)
    this->InsituLink->Delete();
}

const PyFRData*
vtkPyFRPipeline::PyData(vtkCPDataDescription* desc) const {
  vtkPyFRData* pyfrData =
    vtkPyFRData::SafeDownCast(desc->
                              GetInputDescriptionByName("input")->GetGrid());
  return pyfrData->GetData();
}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::Initialize(const char* hostName, int port, char* fileName,
                                 vtkCPDataDescription* dataDescription)
{
  this->view_to_coprocess = 0;
  this->FileName = std::string(fileName);

  vtkSMProxyManager* proxyManager = vtkSMProxyManager::GetProxyManager();

  // Load PyFR plugin
#ifdef PYFR_SINGLE
PV_PLUGIN_IMPORT(pyfr_plugin_fp32)
#else
PV_PLUGIN_IMPORT(pyfr_plugin_fp64)
#endif

  // Grab the active session proxy manager
  vtkSMSessionProxyManager* sessionProxyManager =
    proxyManager->GetActiveSessionProxyManager();

  // Create the vtkLiveInsituLink (the "link" to the visualization processes).
  this->InsituLink = vtkLiveInsituLink::New();

  // Tell vtkLiveInsituLink what host/port must it connect to for the
  // visualization process.
  this->InsituLink->SetHostname(hostName);
  this->InsituLink->SetInsituPort(port);

  // Grab the data object from the data description
  vtkPyFRData* pyfrData =
    vtkPyFRData::SafeDownCast(dataDescription->
                              GetInputDescriptionByName("input")->GetGrid());

  // If these flags are set to true, the data will be written to vtk files on
  // the server side, but the pipeline cannot be cannected to a client.
  bool preFilterWrite = false;
  bool postFilterWrite = false;

  // Construct a pipeline controller to register my elements
  typedef vtkSMParaViewPipelineControllerWithRendering ctlr_;
  this->controller = vtkSmartPointer<ctlr_>::New();

  // Create a vtkPVTrivialProducer and set its output
  // to be the input data.
  vtkSmartPointer<vtkSMSourceProxy> producer;
  producer.TakeReference(
    vtkSMSourceProxy::SafeDownCast(
      sessionProxyManager->NewProxy("sources", "PVTrivialProducer")));
  producer->UpdateVTKObjects();
  vtkObjectBase* clientSideObject = producer->GetClientSideObject();
  vtkPVTrivialProducer* realProducer =
    vtkPVTrivialProducer::SafeDownCast(clientSideObject);
  realProducer->SetOutput(pyfrData);
  this->controller->InitializeProxy(producer);
  this->controller->RegisterPipelineProxy(producer,"Source");

  if (preFilterWrite)
    {
    // Create a convertor to convert the pyfr data into a vtkUnstructuredGrid
    vtkSmartPointer<vtkSMSourceProxy> pyfrDataConverter;
    pyfrDataConverter.TakeReference(
      vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                     NewProxy("filters", "PyFRDataConverter")));
    vtkSMInputProperty* pyfrDataConverterInputConnection =
      vtkSMInputProperty::SafeDownCast(pyfrDataConverter->GetProperty("Input"));

    producer->UpdateVTKObjects();
    pyfrDataConverterInputConnection->SetInputConnection(0, producer, 0);
    pyfrDataConverter->UpdatePropertyInformation();
    pyfrDataConverter->UpdateVTKObjects();
    this->controller->InitializeProxy(pyfrDataConverter);
    this->controller->RegisterPipelineProxy(pyfrDataConverter,
                                            "convertPyFRData");

    // Create an unstructured grid writer, set the filename and then update the
    // pipeline.
    vtkSmartPointer<vtkSMWriterProxy> unstructuredGridWriter;
    unstructuredGridWriter.TakeReference(
      vtkSMWriterProxy::SafeDownCast(sessionProxyManager->
                                     NewProxy("writers",
                                              "XMLUnstructuredGridWriter")));
    vtkSMInputProperty* unstructuredGridWriterInputConnection =
      vtkSMInputProperty::SafeDownCast(unstructuredGridWriter->
                                       GetProperty("Input"));
    unstructuredGridWriterInputConnection->SetInputConnection(0,
                                                              pyfrDataConverter,
                                                              0);
    vtkSMStringVectorProperty* unstructuredGridFileName =
      vtkSMStringVectorProperty::SafeDownCast(unstructuredGridWriter->
                                              GetProperty("FileName"));

      {
      std::ostringstream o;
      o << this->FileName.substr(0,this->FileName.find_last_of("."));
      o << "_" <<std::fixed << std::setprecision(3)<<dataDescription->GetTime();
      o << ".vtu";
      unstructuredGridFileName->SetElement(0, o.str().c_str());
      }

      unstructuredGridWriter->UpdatePropertyInformation();
      unstructuredGridWriter->UpdateVTKObjects();
      unstructuredGridWriter->UpdatePipeline();
      this->controller->InitializeProxy(unstructuredGridWriter);
      this->controller->RegisterPipelineProxy(unstructuredGridWriter,
                                        "UnstructuredGridWriter");
    }

  // Add the merge points filter
  vtkSmartPointer<vtkSMSourceProxy> mergePoints;
  mergePoints.TakeReference(
    vtkSMSourceProxy::SafeDownCast(
      sessionProxyManager->NewProxy("filters", "PyFRMergePointsFilter")));
  controller->PreInitializeProxy(mergePoints);
  vtkSMPropertyHelper(mergePoints, "Input").Set(producer, 0);
  mergePoints->UpdateVTKObjects();
  controller->PostInitializeProxy(mergePoints);
  controller->RegisterPipelineProxy(mergePoints,"MergePoints");


  // Add the gradient filter
  vtkSmartPointer<vtkSMSourceProxy> gradients;
  gradients.TakeReference(
    vtkSMSourceProxy::SafeDownCast(
      sessionProxyManager->NewProxy("filters", "PyFRGradientFilter")));
  controller->PreInitializeProxy(gradients);
  vtkSMPropertyHelper(gradients, "Input").Set(mergePoints, 0);
  gradients->UpdateVTKObjects();
  controller->PostInitializeProxy(gradients);
  controller->RegisterPipelineProxy(gradients,"Gradients");

  // Create views
  vtkSmartPointer<vtkSMViewProxy> polydataViewer1;
  polydataViewer1.TakeReference(
    vtkSMViewProxy::SafeDownCast(sessionProxyManager->
                                 NewProxy("views","RenderView")));
  this->controller->InitializeProxy(polydataViewer1);
  this->controller->RegisterViewProxy(polydataViewer1);

  this->ActiveMapper1 = vtkSmartPointer<vtkPyFRMapper>::New();
  this->ActiveMapper3 = vtkSmartPointer<vtkPyFRMapper>::New();

  //Add the components of PipelineMode1
    {
    this->InitPipeline1(gradients, dataDescription );
    vtkAddActor(this->ActiveMapper1, this->Contour1, polydataViewer1);
    vtkAddActor(this->ActiveMapper3, this->Contour2, polydataViewer1, -0.955);
    }


  vtkSmartPointer<vtkSMViewProxy> polydataViewer2;
  polydataViewer2.TakeReference(
    vtkSMViewProxy::SafeDownCast(sessionProxyManager->
                                 NewProxy("views","RenderView")));
  this->controller->InitializeProxy(polydataViewer2);
  this->controller->RegisterViewProxy(polydataViewer2);

  this->ActiveMapper2 = vtkSmartPointer<vtkPyFRMapper>::New();

    {
    //Add the components of PipelineMode2
    this->InitPipeline2(gradients, dataDescription);
    vtkAddActor(this->ActiveMapper2, this->Slice, polydataViewer2);
    }

  if (postFilterWrite)
    {
    this->DumpToFile(dataDescription);
    }

  const bool plane = true;
  if(plane)
    {
    vtkSmartPointer<vtkSMSourceProxy> airplane;
    airplane.TakeReference(
      vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                     NewProxy("internal_sources",
                                              "XMLUnstructuredGridReaderCore")));
    this->controller->PreInitializeProxy(airplane);
      {
      std::ostringstream o;
      const char* ddir = getenv("PYFR_DATA_DIR");
      if(NULL == ddir)
        {
        std::cerr << "Please set the PYFR_DATA_DIR environment variable to "
                     "the directory containing wall.vtu.\n";
        ddir = "/lustre/atlas2/ard116/proj-shared/Test/T106D_cascade_3d-1-105.600PCC-001RCPLDG/TR1/wall";
        std::cerr << "Assuming " << ddir << " for now ...\n";
        }
      o << ddir << "/wall.vtu";
      vtkSMPropertyHelper(airplane, "FileName").Set(o.str().c_str());
      }
    airplane->UpdateVTKObjects();
    this->controller->PostInitializeProxy(airplane);
    this->controller->RegisterPipelineProxy(airplane,"Airplane");

    // Create a view
    vtkSmartPointer<vtkDataSetMapper> airplaneMapper =
      vtkSmartPointer<vtkDataSetMapper>::New();
    vtkAddActor(airplaneMapper, airplane, polydataViewer1);
    }

  // Create a timestamp
  //this->Timestamp = vtkSmartPointer<vtkTextActor>::New();
  //  {
  //  std::ostringstream o;
  //  o << dataDescription->GetTime()<<" s";
  //  this->Timestamp->SetInput(o.str().c_str());
  //  }
  //this->Timestamp->GetTextProperty()->SetBackgroundColor(0.,0.,0.);
  //this->Timestamp->GetTextProperty()->SetBackgroundOpacity(1);

  // Add the actors to the renderer, set the background and size
  //{
  //vtkSMRenderViewProxy* rview = vtkSMRenderViewProxy::SafeDownCast(polydataViewer1);

  //vtkRenderer* ren = rview->GetRenderer();
  //ren->AddActor2D(this->Timestamp);
  //rview->UpdateVTKObjects();
  //}

  // Initialize the "link"
  this->InsituLink->InsituInitialize(vtkSMProxyManager::GetProxyManager()->
                                     GetActiveSessionProxyManager());
  this->SetSpecularLighting(0, 0, 0);
  this->SetSpecularLighting(0, 0, 1);

}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::InitPipeline1(vtkSmartPointer<vtkSMSourceProxy> input,
                                    vtkCPDataDescription* dataDescription)
{
  vtkSMProxyManager* proxyManager = vtkSMProxyManager::GetProxyManager();
  // Grab the active session proxy manager
  vtkSMSessionProxyManager* sessionProxyManager =
    proxyManager->GetActiveSessionProxyManager();

  // Grab the data object from the data description
  vtkPyFRData* pyfrData =
    vtkPyFRData::SafeDownCast(dataDescription->
                              GetInputDescriptionByName("input")->GetGrid());

    
    
    
    // Add the first clip filter
    this->Clip1.TakeReference(
        vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                       NewProxy("filters",
                                               "PyFRCrinkleClipFilter")));
    controller->PreInitializeProxy(this->Clip1);
    vtkSMPropertyHelper(this->Clip1, "Input").Set(input, 0);
    this->Clip1->UpdateVTKObjects();
    this->controller->PostInitializeProxy(this->Clip1);
    this->controller->RegisterPipelineProxy(this->Clip1,"Clip");

    // Add the second clip filter
    this->Clip2.TakeReference(
        vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                       NewProxy("filters",
                                               "PyFRCrinkleClipFilter")));
    controller->PreInitializeProxy(this->Clip2);
    vtkSMPropertyHelper(this->Clip2, "Input").Set(this->Clip1, 0);
    this->Clip2->UpdateVTKObjects();
    this->controller->PostInitializeProxy(this->Clip2);
    this->controller->RegisterPipelineProxy(this->Clip2,"Clip");
  
  
  // Add the contour filter
  this->Contour1.TakeReference(
    vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                   NewProxy("filters",
                                            "PyFRContourFilter")));
  /*
  vtkSMInputProperty* contourInputConnection =
    vtkSMInputProperty::SafeDownCast(this->Contour1->GetProperty("Input"));
*/
  this->controller->PreInitializeProxy(this->Contour1);

  vtkSMPropertyHelper(this->Contour1, "Input").Set(this->Clip2, 0);
  vtkSMPropertyHelper(this->Contour1,"ContourField").Set(0);
  vtkSMPropertyHelper(this->Contour1,"ColorField").Set(8);

  // Set up the isovalues to use.
  const std::vector<float> isovalues = pyfrData->GetData()->isovalues();
  for(size_t i=0; i < isovalues.size(); ++i) {
    root(printf("[catalyst] setting isovalue %zu: %g\n", i, isovalues[i]));
    vtkSMPropertyHelper(this->Contour1, "ContourValues").Set(i, isovalues[i]);
  }
  this->Contour1->UpdateVTKObjects();
  this->controller->PostInitializeProxy(this->Contour1);
  this->controller->RegisterPipelineProxy(this->Contour1,"Contour1");
    

    // Add the third clip filter
    this->Clip3.TakeReference(
        vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                       NewProxy("filters",
                                               "PyFRCrinkleClipFilter")));
    controller->PreInitializeProxy(this->Clip3);
    vtkSMPropertyHelper(this->Clip3, "Input").Set(input, 0);
    this->Clip3->UpdateVTKObjects();
    this->controller->PostInitializeProxy(this->Clip3);
    this->controller->RegisterPipelineProxy(this->Clip3,"Clip");

    // Add the fourth clip filter
    this->Clip4.TakeReference(
        vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                       NewProxy("filters",
                                               "PyFRCrinkleClipFilter")));
    controller->PreInitializeProxy(this->Clip4);
    vtkSMPropertyHelper(this->Clip4, "Input").Set(this->Clip3, 0);
    this->Clip4->UpdateVTKObjects();
    this->controller->PostInitializeProxy(this->Clip4);
    this->controller->RegisterPipelineProxy(this->Clip4,"Clip");

    // Add the contour filter
    this->Contour2.TakeReference(
    vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                   NewProxy("filters",
                                            "PyFRContourFilter")));
  /*
  vtkSMInputProperty* contourInputConnection =
    vtkSMInputProperty::SafeDownCast(this->Contour2->GetProperty("Input"));
  */
  this->controller->PreInitializeProxy(this->Contour2);

  vtkSMPropertyHelper(this->Contour2, "Input").Set(this->Clip4, 0);
  vtkSMPropertyHelper(this->Contour2,"ContourField").Set(0);
  vtkSMPropertyHelper(this->Contour2,"ColorField").Set(8);

  // Set up the isovalues to use.
  for(size_t i=0; i < isovalues.size(); ++i) {
    root(printf("[catalyst] setting isovalue %zu: %g\n", i, isovalues[i]));
    vtkSMPropertyHelper(this->Contour2, "ContourValues").Set(i, isovalues[i]);
  }
  this->Contour2->UpdateVTKObjects();
  this->controller->PostInitializeProxy(this->Contour2);
  this->controller->RegisterPipelineProxy(this->Contour2,"Contour2");

}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::InitPipeline2(vtkSmartPointer<vtkSMSourceProxy> input,
                                    vtkCPDataDescription*)
{
  vtkSMProxyManager* proxyManager = vtkSMProxyManager::GetProxyManager();
  // Grab the active session proxy manager
  vtkSMSessionProxyManager* sessionProxyManager =
    proxyManager->GetActiveSessionProxyManager();

  // Add the slice filter
  this->Slice.TakeReference(
    vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                   NewProxy("filters",
                                            "PyFRParallelSliceFilter")));
  this->controller->PreInitializeProxy(this->Slice);
  vtkSMPropertyHelper(this->Slice, "Input").Set(input, 0);
  vtkSMPropertyHelper(this->Slice,"ColorField").Set(8);
  this->Slice->UpdateVTKObjects();
  this->controller->PostInitializeProxy(this->Slice);
  this->controller->RegisterPipelineProxy(this->Slice,"Slice");

}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::DumpToFile(vtkCPDataDescription* dataDescription)
{
  vtkSMProxyManager* proxyManager = vtkSMProxyManager::GetProxyManager();
  // Grab the active session proxy manager
  vtkSMSessionProxyManager* sessionProxyManager =
    proxyManager->GetActiveSessionProxyManager();

  // Create a converter to convert the pyfr contour data into polydata
  vtkSmartPointer<vtkSMSourceProxy> pyfrContourDataConverter;
  pyfrContourDataConverter.TakeReference(
    vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                   NewProxy("filters",
                                            "PyFRContourDataConverter")));
  vtkSMInputProperty* pyfrContourDataConverterInputConnection =
    vtkSMInputProperty::SafeDownCast(pyfrContourDataConverter->
                                     GetProperty("Input"));

  pyfrContourDataConverterInputConnection->SetInputConnection(0, this->Contour1, 0);
  pyfrContourDataConverter->UpdatePropertyInformation();
  pyfrContourDataConverter->UpdateVTKObjects();
  this->controller->InitializeProxy(pyfrContourDataConverter);
  this->controller->RegisterPipelineProxy(pyfrContourDataConverter,
                                          "ConvertContoursToPolyData");

  // Create the polydata writer, set the filename and then update the pipeline
  vtkSmartPointer<vtkSMWriterProxy> polydataWriter;
  polydataWriter.TakeReference(
    vtkSMWriterProxy::SafeDownCast(sessionProxyManager->
                                   NewProxy("writers", "XMLPolyDataWriter")));
  vtkSMInputProperty* polydataWriterInputConnection =
    vtkSMInputProperty::SafeDownCast(polydataWriter->GetProperty("Input"));
  polydataWriterInputConnection->SetInputConnection(0,
                                                    pyfrContourDataConverter,
                                                    0);
  vtkSMStringVectorProperty* polydataFileName =
    vtkSMStringVectorProperty::SafeDownCast(polydataWriter->
                                            GetProperty("FileName"));

    {
    std::ostringstream o;
    o << this->FileName.substr(0,this->FileName.find_last_of("."));
    o << "_"<<std::fixed<<std::setprecision(3)<<dataDescription->GetTime();
    o << ".vtp";
    polydataFileName->SetElement(0, o.str().c_str());
    }

    polydataWriter->UpdatePropertyInformation();
    polydataWriter->UpdateVTKObjects();
    polydataWriter->UpdatePipeline();
    this->controller->InitializeProxy(polydataWriter);
    this->controller->RegisterPipelineProxy(polydataWriter,"polydataWriter");
}

//----------------------------------------------------------------------------
int vtkPyFRPipeline::RequestDataDescription(
  vtkCPDataDescription* dataDescription)
{
  if(!dataDescription)
    {
    vtkWarningMacro("dataDescription is NULL.");
    return 0;
    }

  if(this->FileName.empty())
    {
    vtkWarningMacro("No output file name given to output results to.");
    return 0;
    }

  dataDescription->GetInputDescriptionByName("input")->AllFieldsOn();
  dataDescription->GetInputDescriptionByName("input")->GenerateMeshOn();
  return 1;
}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::SetResolution(uint32_t width, uint32_t height)
{
  vtkSMSessionProxyManager* sessionProxyManager =
    vtkSMProxyManager::GetProxyManager()->GetActiveSessionProxyManager();
  vtkNew<vtkCollection> views;
  sessionProxyManager->GetProxies("views",views.GetPointer());
  const size_t nviews = views->GetNumberOfItems();
  for (int i=0; i < nviews; i++)
    {
    vtkSMViewProxy* viewProxy =
      vtkSMViewProxy::SafeDownCast(views->GetItemAsObject(i));
    vtkVector2i isz(width, height);
    vtkSMPropertyHelper(viewProxy, "ViewSize").Set(isz.GetData(), 2);
    }
}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::SetSpecularLighting(float coefficient, float power, int view)
{
  vtkSMSessionProxyManager* sessionProxyManager =
    vtkSMProxyManager::GetProxyManager()->GetActiveSessionProxyManager();
  vtkNew<vtkCollection> views;
  sessionProxyManager->GetProxies("views",views.GetPointer());
  const size_t nviews = views->GetNumberOfItems();
  //for (int i=0; i < nviews; i++)
    {
    vtkSMViewProxy* viewProxy =
      vtkSMViewProxy::SafeDownCast(views->GetItemAsObject(view));
    vtkSMRenderViewProxy* rview = vtkSMRenderViewProxy::SafeDownCast(viewProxy);
    vtkRenderer* ren = rview->GetRenderer();

    vtkActorCollection* actors = ren->GetActors();
    vtkCollectionSimpleIterator ait;
    actors->InitTraversal(ait);

    vtkActor *actor;
    while ( (actor = actors->GetNextActor(ait)) )
      {
      actor->GetProperty()->SetSpecular(coefficient);
      actor->GetProperty()->SetSpecularPower(power);
      }
    }
}

void fillRuntimeVectors(int pipeline, const uint8_t* rgba, const float* loc, size_t n);
//----------------------------------------------------------------------------
void vtkPyFRPipeline::SetColorTable(const uint8_t* rgba, const float* loc,
                                    size_t n, int pipeline)
{
  // -1 is ColorTable::RUNTIME; we can't include that header.
  //std::cout << " vtkPyFRPipeline::SetColorTable p: " << WhichPipeline << std::endl;
  if(pipeline <= 1)
  {
    fillRuntimeVectors(1, rgba, loc, n);
    vtkObjectBase* obj1 = this->Contour1->GetClientSideObject();
    vtkPyFRContourFilter* filt1 = vtkPyFRContourFilter::SafeDownCast(obj1);
    filt1->SetColorPalette(-1);

    
    
    vtkObjectBase* obj2 = this->Contour2->GetClientSideObject();
    vtkPyFRContourFilter* filt2 = vtkPyFRContourFilter::SafeDownCast(obj1);
    filt2->SetColorPalette(-1);
    
    
  }
  else if(pipeline == 2)
  {
    fillRuntimeVectors(2, rgba, loc, n);
    vtkObjectBase* obj = this->Slice->GetClientSideObject();
    vtkPyFRParallelSliceFilter* filt = vtkPyFRParallelSliceFilter::SafeDownCast(obj);
    filt->SetColorPalette(-1);
  }
}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::SetColorRange(FPType low, FPType high, int pipeline)
{
  if(pipeline <= 1)
  {
    vtkObjectBase* obj1 = this->Contour1->GetClientSideObject();
    vtkPyFRContourFilter* filt1 = vtkPyFRContourFilter::SafeDownCast(obj1);
    filt1->SetColorRange(low,high);
    
    vtkObjectBase* obj2 = this->Contour2->GetClientSideObject();
    vtkPyFRContourFilter* filt2 = vtkPyFRContourFilter::SafeDownCast(obj2);
    filt2->SetColorRange(low,high);
    
    
  }
  else if(pipeline == 2)
  {
    vtkObjectBase* obj = this->Slice->GetClientSideObject();
    vtkPyFRParallelSliceFilter* filt = vtkPyFRParallelSliceFilter::SafeDownCast(obj);
    filt->SetColorRange(low,high);
  }
}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::SetFieldToColorBy(int field, int pipeline)
{
  if(pipeline <= 1)
  {
    vtkSMPropertyHelper(this->Contour1,"ColorField").Set(field);
    this->Contour1->UpdatePropertyInformation();
    this->Contour1->UpdateVTKObjects();
    
    vtkSMPropertyHelper(this->Contour2,"ColorField").Set(field);
    this->Contour2->UpdatePropertyInformation();
    this->Contour2->UpdateVTKObjects(); 
    
  }
  else if(pipeline == 2)
  {
    vtkSMPropertyHelper(this->Slice,"ColorField").Set(field);
    this->Slice->UpdatePropertyInformation();
    this->Slice->UpdateVTKObjects();
  }
}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::SetFieldToContourBy(int field)
{
  //if(this->WhichPipeline <= 1)
  {
    vtkSMPropertyHelper(this->Contour1,"ContourField").Set(field);
    this->Contour1->UpdatePropertyInformation();
    this->Contour1->UpdateVTKObjects();
    
    vtkSMPropertyHelper(this->Contour2,"ContourField").Set(field);
    this->Contour2->UpdatePropertyInformation();
    this->Contour2->UpdateVTKObjects();
    
    
  }
}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::SetSlicePlanes(float origin[3], float normal[3],
                                     int number, double spacing)
{
    for(int i=0; i < 3; ++i)
      {
      vtkSMPropertyHelper(this->Slice,"Origin").Set(i, origin[i]);
      vtkSMPropertyHelper(this->Slice,"Normal").Set(i, normal[i]);
      }
    vtkSMPropertyHelper(this->Slice,"NumberOfPlanes").Set(number);
    vtkSMPropertyHelper(this->Slice,"Spacing").Set(spacing);
    this->Slice->UpdatePropertyInformation();
    this->Slice->UpdateVTKObjects();
}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::SetClipPlanes(float origin1[3], float normal1[3],
                                    float pitch)
{
    this->pitch = pitch;
  for(int i=0; i < 3; ++i)
    {
    vtkSMPropertyHelper(this->Clip1,"Origin").Set(i, origin1[i]);
    vtkSMPropertyHelper(this->Clip1,"Normal").Set(i, normal1[i]);
    normal1[i] *= -1;
    }
  origin1[1] += pitch;
  for(int i=0; i < 3; ++i)
    {
    vtkSMPropertyHelper(this->Clip2,"Origin").Set(i, origin1[i]);
    vtkSMPropertyHelper(this->Clip2,"Normal").Set(i, normal1[i]);
    normal1[i] *= -1;
    }
    

  for(int i=0; i < 3; ++i)
    {
    vtkSMPropertyHelper(this->Clip3,"Origin").Set(i, origin1[i]);
    vtkSMPropertyHelper(this->Clip3,"Normal").Set(i, normal1[i]);
    normal1[i] *= -1;
    }
  origin1[1] += pitch;
  for(int i=0; i < 3; ++i)
    {
    vtkSMPropertyHelper(this->Clip4,"Origin").Set(i, origin1[i]);
    vtkSMPropertyHelper(this->Clip4,"Normal").Set(i, normal1[i]);
    normal1[i] *= -1;
    }


    
    
    this->Clip1->UpdatePropertyInformation();
    this->Clip1->UpdateVTKObjects();
  
    this->Clip2->UpdatePropertyInformation();
    this->Clip2->UpdateVTKObjects();
    
    this->Clip3->UpdatePropertyInformation();
    this->Clip3->UpdateVTKObjects();
  
    this->Clip4->UpdatePropertyInformation();
    this->Clip4->UpdateVTKObjects();

    
}

//----------------------------------------------------------------------------
int vtkPyFRPipeline::CoProcess(vtkCPDataDescription* dataDescription)
{
  vtkSMSessionProxyManager* sessionProxyManager =
    vtkSMProxyManager::GetProxyManager()->GetActiveSessionProxyManager();

  // Grab the data object from the data description
  vtkPyFRData* pyfrData =
    vtkPyFRData::SafeDownCast(dataDescription->
                              GetInputDescriptionByName("input")->GetGrid());

  // Use it to update the source
  vtkSMSourceProxy* source =
    vtkSMSourceProxy::SafeDownCast(sessionProxyManager->GetProxy("Source"));
  vtkObjectBase* clientSideObject = source->GetClientSideObject();
  vtkPVTrivialProducer* realProducer =
    vtkPVTrivialProducer::SafeDownCast(clientSideObject);
  realProducer->SetOutput(pyfrData,dataDescription->GetTime());

    //{
    //std::ostringstream o;
    //o << dataDescription->GetTime()<<" s";
    //this->Timestamp->SetInput(o.str().c_str());
    //}

  vtkSMSourceProxy* unstructuredGridWriter =
    vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                   GetProxy("UnstructuredGridWriter"));
  if (unstructuredGridWriter)
    {
    vtkSMStringVectorProperty* unstructuredGridFileName =
      vtkSMStringVectorProperty::SafeDownCast(unstructuredGridWriter->
                                              GetProperty("FileName"));
      {
      std::ostringstream o;
      o << this->FileName.substr(0,this->FileName.find_last_of("."));
      o << "_"<<std::fixed<<std::setprecision(3)<<dataDescription->GetTime();
      o << ".vtu";
      unstructuredGridFileName->SetElement(0, o.str().c_str());
      }
      unstructuredGridWriter->UpdatePropertyInformation();
      unstructuredGridWriter->UpdateVTKObjects();
      unstructuredGridWriter->UpdatePipeline();
    }
  vtkSMSourceProxy* polydataWriter =
    vtkSMSourceProxy::SafeDownCast(sessionProxyManager->
                                   GetProxy("polydataWriter"));
  if (polydataWriter)
    {
    vtkSMStringVectorProperty* polydataFileName =
      vtkSMStringVectorProperty::SafeDownCast(polydataWriter->
                                              GetProperty("FileName"));

      {
      std::ostringstream o;
      o << this->FileName.substr(0,this->FileName.find_last_of("."));
      o << "_"<<std::fixed<<std::setprecision(3)<<dataDescription->GetTime();
      o << ".vtp";
      polydataFileName->SetElement(0, o.str().c_str());
      }
      polydataWriter->UpdatePropertyInformation();
      polydataWriter->UpdateVTKObjects();
      polydataWriter->UpdatePipeline();
    }

  // stay in the loop while the simulation is paused
  while (true)
    {
    this->InsituLink->InsituUpdate(dataDescription->GetTime(),
                                   dataDescription->GetTimeStep());

      vtkUpdateFilter(this->Clip1, dataDescription->GetTime());
      vtkUpdateFilter(this->Clip2, dataDescription->GetTime());
      
      vtkUpdateFilter(this->Clip3, dataDescription->GetTime());
      vtkUpdateFilter(this->Clip4, dataDescription->GetTime());
      
      
      vtkUpdateFilter(this->Contour1, dataDescription->GetTime());
      
      vtkUpdateFilter(this->Contour2, dataDescription->GetTime());
      
      vtkUpdateFilter(this->Slice, dataDescription->GetTime());

    if(this->PyData(dataDescription)->PrintMetadata()) {
        double* bds = this->ActiveMapper1->GetBounds();
        reduce(&bds[0], 1, vtkCommunicator::MIN_OP);
        reduce(&bds[1], 1, vtkCommunicator::MAX_OP);
        reduce(&bds[2], 1, vtkCommunicator::MIN_OP);
        reduce(&bds[3], 1, vtkCommunicator::MAX_OP);
        reduce(&bds[4], 1, vtkCommunicator::MIN_OP);
        reduce(&bds[5], 1, vtkCommunicator::MAX_OP);
        std::ostringstream b;
        b << "[catalyst] world bounds: [ ";
        std::copy(bds, bds+6, std::ostream_iterator<double>(b, ", "));
        b << "]\n";
        root(std::cout << b.str());

        b.str("");
        b.clear();
        //if(this->WhichPipeline <= 1)
          {
          vtkObjectBase* obj = this->Contour1->GetClientSideObject();
          const vtkPyFRContourFilter* filt =
              vtkPyFRContourFilter::SafeDownCast(obj);
              
          std::pair<float,float> mm = filt->Range();
          reduce(&mm.first, 1, vtkCommunicator::MIN_OP);
          reduce(&mm.second, 1, vtkCommunicator::MAX_OP);
          b << "[catalyst] range: " << mm.first << "--" << mm.second << "\n";
          }
        root(std::cout << b.str());
    }

    vtkNew<vtkCollection> views;
    sessionProxyManager->GetProxies("views",views.GetPointer());
    const size_t nviews = views->GetNumberOfItems();
    int view = this->view_to_coprocess;
    //for (int view=0; view < nviews; view++)
      {
      vtkSMViewProxy* viewProxy =
        vtkSMViewProxy::SafeDownCast(views->GetItemAsObject(view));
      vtkSMPropertyHelper(viewProxy,"ViewTime").Set(dataDescription->GetTime());
      viewProxy->UpdateVTKObjects();
      viewProxy->Update();

      const PyFRData* pyd = this->PyData(dataDescription);
      vtkWeakPointer<vtkRenderWindow> rw = viewProxy->GetRenderWindow();
      vtkWeakPointer<vtkRendererCollection> rens = rw->GetRenderers();
      assert(rens);
      for(int j=0;j<rens->GetNumberOfItems();j++)
      {
          printf("Renderer: %d\n", j);
          vtkWeakPointer<vtkRenderer> ren = vtkRenderer::SafeDownCast(rens->GetItemAsObject(j));
          ren->ResetCamera();
          ren->ResetCameraClippingRange();
          vtkWeakPointer<vtkCamera> cam = ren->GetActiveCamera();
          const float* eye = pyd->eye;
          if(!std::isnan(eye[0]) && !std::isnan(eye[1]) && !std::isnan(eye[2])) {
            cam->SetPosition(eye[0], eye[1], eye[2]);
          }
          const float* ref = pyd->ref;
          if(!std::isnan(ref[0]) && !std::isnan(ref[1]) && !std::isnan(ref[2])) {
            cam->SetFocalPoint(ref[0], ref[1], ref[2]);
          }
          const float* vup = pyd->vup;
          if(!std::isnan(vup[0]) && !std::isnan(vup[1]) && !std::isnan(vup[2])) {
            cam->SetViewUp(vup[0], vup[1], vup[2]);
          }
          const float* bg = pyd->bg_color;
          if(!std::isnan(bg[0]) && !std::isnan(bg[1]) && !std::isnan(!bg[2])) {
            ren->SetBackground(bg[0], bg[1], bg[2]);
          }
          root(
            if(this->PyData(dataDescription)->PrintMetadata()) {
              output_camera(cam);
            }
          );
      }

      const int magnification = 1;
      const int quality = 100;
      char fname[128] = {0};
      if(nviews > 1)
        {
        snprintf(fname, 128, "%s%04ld-v%d.png", pyd->fnprefix.c_str(),
                 (long)dataDescription->GetTimeStep(), view);
        } else {
        snprintf(fname, 128, "%s%04ld.png", pyd->fnprefix.c_str(),
                 (long)dataDescription->GetTimeStep());
        }
      printf("View: %d\n", view);

      printf("Saving: %s\n", fname);
      this->controller->WriteImage(viewProxy, fname, magnification, quality);
      }

    this->InsituLink->InsituPostProcess(dataDescription->GetTime(),
                                        dataDescription->GetTimeStep());
    if (this->InsituLink->GetSimulationPaused())
      {
      if (this->InsituLink->WaitForLiveChange())
        {
        break;
        }
      }
    else
      {
      break;
      }
    }

  return 1;
}

//----------------------------------------------------------------------------
void vtkPyFRPipeline::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "FileName: " << this->FileName << "\n";
}
