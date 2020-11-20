//------------------------------------------------------------------------------
//  ngltfexporter.cc
//  (C) 2020 Individual contributors, see AUTHORS file
//------------------------------------------------------------------------------
#include "stdneb.h"
#include "ngltfexporter.h"
#include "io/ioserver.h"
#include "meshutil/meshbuildersaver.h"
#include "modelutil/modeldatabase.h"
#include "modelutil/modelattributes.h"
#include "toolkitutil/animutil/animbuildersaver.h"
#include "toolkitutil/skeletonutil/skeletonbuilder.h"
#include "toolkitutil/skeletonutil/skeletonbuildersaver.h"
#include "util/bitfield.h"
#include "timing/timer.h"
#include "io/binarywriter.h"
#include "io/filestream.h"
#include "surface/surfacebuilder.h"

#define NEBULA_VALIDATE_GLTF 1

using namespace Util;
namespace ToolkitUtil
{
__ImplementClass(ToolkitUtil::NglTFExporter, 'glte', Base::ExporterBase);

//------------------------------------------------------------------------------
/**
*/
NglTFExporter::NglTFExporter() :
	scaleFactor(1.0f),
	exportMode(Static),
	exportFlags(ToolkitUtil::FlipUVs)
{
}

//------------------------------------------------------------------------------
/**
*/
NglTFExporter::~NglTFExporter()
{
	// empty
}

//------------------------------------------------------------------------------
/**
*/
void NglTFExporter::Open()
{
	ExporterBase::Open();

	this->scene = NglTFScene::Create();
	this->scene->Open();
}

//------------------------------------------------------------------------------
/**
*/
void NglTFExporter::Close()
{
	ExporterBase::Close();

	this->scene->Close();
	this->scene = nullptr;
}

//------------------------------------------------------------------------------
/**
*/
bool NglTFExporter::StartExport(const IO::URI & file)
{
	n_assert(this->isOpen);
	IO::IoServer* ioServer = IO::IoServer::Instance();

	String localPath = file.GetHostAndLocalPath();
	this->file = localPath;
	String exportType = this->exportMode == ToolkitUtil::Static ? "static" : "character";
	n_printf("Exporting assets as %s:\n        %s\n", exportType.AsCharPtr(), localPath.AsCharPtr());

	// we want to see if the model file exists, because that's the only way to know if ALL the resources are old or new...
	if (!this->NeedsConversion(localPath))
	{
		n_printf("    [File has not changed, ignoring export]\n\n", localPath.AsCharPtr());
		return false;
	}

	bool res = this->gltfScene.Deserialize(file);

	if (!res)
	{
		n_warning("WARNING: Failed to import '%s'\n\n", localPath.AsCharPtr());
		return false;
	}

	// deduct file name from URL
	String fileName = localPath.ExtractFileName();
	String fileExtension = fileName.GetFileExtension();
	fileName.StripFileExtension();
	String catName = localPath.ExtractLastDirName();

	if (gltfScene.images.Size() > 0)
	{
		// Export embedded textures to file-specific directory
		bool hasEmbedded = false;
		String embeddedPath = "tex:" + catName + "/" + fileName + "_" + fileExtension;
		if (IO::IoServer::Instance()->DirectoryExists(embeddedPath))
		{
			// delete all previously generated images
			if (!IO::IoServer::Instance()->DeleteDirectory(embeddedPath))
				n_warning("Warning: NglTFExporter: Could not delete old directory for embedded gltf images.\n");
		}

		for (IndexT i = 0; i < gltfScene.images.Size(); i++)
		{
			if (gltfScene.images[i].embedded)
			{
				hasEmbedded = true;
				break;
			}
		}

		if (hasEmbedded)
		{
			IO::IoServer::Instance()->CreateDirectory(embeddedPath);
			
			// export all embedded images
			for (IndexT i = 0; i < gltfScene.images.Size(); i++)
			{
				Gltf::Image const& image = gltfScene.images[i];
				if (!image.embedded)
					continue;

				Util::Blob const* data;

				if (!image.uri.IsEmpty())
				{
					data = &image.data;
				}
				else
				{
					// TODO: Untested!
					auto const& bufferView = gltfScene.bufferViews[image.bufferView];
					data = &gltfScene.buffers[bufferView.buffer].data;
				}

				// create temp directory from guid. so that other jobs won't interfere
				Guid guid;
				guid.Generate();
				String tmpDir;
				tmpDir.Format("%s/%s", "temp:textureconverter", guid.AsString().AsCharPtr());

				Util::String format = (image.type == Gltf::Image::Type::Jpg) ? ".jpg" : ".png";
				// export the content of blob to a temporary file
				Ptr<IO::BinaryWriter> writer = IO::BinaryWriter::Create();
				Util::String intermediateDir = tmpDir + "/" + catName + "/" + fileName + "_" + fileExtension;
				Util::String intermediateFile = intermediateDir + "/" + Util::String::FromInt(i) + format;
				IO::IoServer::Instance()->CreateDirectory(intermediateDir);
				writer->SetStream(IO::IoServer::Instance()->CreateStream(intermediateFile));
				if (!writer->Open())
				{
					n_warning("Warning: NglTFExporter: Could not open filestream to write intermediate image format.\n");
					return false;
				}
				writer->GetStream()->Write(data->GetPtr(), data->Size());
				writer->Close();

				this->texConverter->SetDstDir("tex:" + catName + "/");
				// content is base 64 encoded in uri
				if (!this->texConverter->ConvertTexture(intermediateFile, tmpDir))
				{
					n_error("ERROR: failed to convert texture\n");
				}

				if (IO::IoServer::Instance()->DirectoryExists(tmpDir))
				{
					if (!IO::IoServer::Instance()->DeleteDirectory(tmpDir))
						n_warning("Warning: NglTFExporter: Could not delete temporary texconverter directory.\n");
				}
			}
		}
	}

	String surfaceExportPath = "sur:" + catName + "/" + fileName + "_" + fileExtension;

	if (gltfScene.materials.Size() > 0)
	{
		if (IO::IoServer::Instance()->DirectoryExists(surfaceExportPath))
		{
			// delete all previously generated images
			if (!IO::IoServer::Instance()->DeleteDirectory(surfaceExportPath))
				n_warning("Warning: NglTFExporter: Could not delete old directory for gltf-specific surfaces.\n");
		}

		Util::String texCatDir = "tex:" + catName;
		Util::String textureDir = texCatDir + "/" + fileName + "_" + fileExtension + "/";
		// Generate surfaces
		for (IndexT i = 0; i < gltfScene.materials.Size(); i++)
		{
			Gltf::Material const& material = gltfScene.materials[i];
			
			SurfaceBuilder builder;
			builder.SetDstDir(surfaceExportPath);
			builder.SetMaterial("GLTF Static");
			if (material.pbrMetallicRoughness.baseColorTexture.index != -1)
			{
				int baseColorTexture = gltfScene.textures[material.pbrMetallicRoughness.baseColorTexture.index].source;
				if (gltfScene.images[baseColorTexture].embedded)
					builder.AddParam("baseColorTexture", textureDir + Util::String::FromInt(baseColorTexture));
				else
				{
					// texture is not embedded, we need to find the correct path to it
					n_error("TODO");
				}
			}
			else
			{
				builder.AddParam("baseColorTexture", "tex:system/white");
			}

			if (material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
			{
				int metallicRoughnessTexture = gltfScene.textures[material.pbrMetallicRoughness.metallicRoughnessTexture.index].source;

				if (gltfScene.images[metallicRoughnessTexture].embedded)
					builder.AddParam("metallicRoughnessTexture", textureDir + Util::String::FromInt(metallicRoughnessTexture));
				else
				{
					// texture is not embedded, we need to find the correct path to it
					n_error("TODO");
				}
			}
			else
			{
				builder.AddParam("metallicRoughnessTexture", "tex:system/white");
			}
			
			int normalTexture = gltfScene.textures[material.normalTexture.index].source;
			builder.AddParam("normalTexture", textureDir + Util::String::FromInt(normalTexture));
			builder.AddParam("baseColorFactor", Util::String::FromVec4(material.pbrMetallicRoughness.baseColorFactor));
			builder.AddParam("metallicFactor", Util::String::FromFloat(material.pbrMetallicRoughness.metallicFactor));
			builder.AddParam("roughnessFactor", Util::String::FromFloat(material.pbrMetallicRoughness.roughnessFactor));
			builder.ExportBinary(surfaceExportPath + "/" + material.name + ".sur");
		}
	}
	
	Timing::Timer timer;
	timer.Start();

	this->scene->SetName(fileName);
	this->scene->SetCategory(catName);
	this->scene->Setup(&this->gltfScene, this->exportFlags, this->exportMode, this->scaleFactor);

	// Flatten hierarchy, and merge all meshes into one
	this->scene->Flatten();

	// format file destination
	String destinationFile;
	destinationFile.Format("msh:%s/%s.nvx2", catName.AsCharPtr(), fileName.AsCharPtr());

	// save mesh to file
	if (false == MeshBuilderSaver::SaveNvx2(IO::URI(destinationFile), *this->scene->GetMesh(), this->platform))
	{
		n_error("Failed to save Nvx2 file: %s\n", destinationFile.AsCharPtr());
	}

	timer.Stop();
	// print info
	n_printf("[Generated graphics mesh: %s] (%.2f ms)\n", destinationFile.AsCharPtr(), timer.GetTime() * 1000.0f);
	
	
	// create scene writer
	this->sceneWriter = NglTFSceneWriter::Create();
	this->sceneWriter->SetSurfaceExportPath(surfaceExportPath);
	this->sceneWriter->SetForce(true);
	this->sceneWriter->SetPlatform(this->platform);
	this->sceneWriter->SetScene(this->scene);

	return true;
}

//------------------------------------------------------------------------------
/**
*/
void NglTFExporter::EndExport()
{
	n_assert(this->IsOpen());

	// format base path
	String basePath;
	basePath.Format("src:assets/%s/", this->file.ExtractLastDirName().AsCharPtr());

	// generate models
	this->sceneWriter->GenerateModels(basePath, this->exportFlags, this->exportMode);
	this->sceneWriter = nullptr;

	// cleanup data
	this->gltfScene = Gltf::Document();
}

//------------------------------------------------------------------------------
/**
*/
void NglTFExporter::ExportFile(const IO::URI & file)
{
	// get directory and file
	String fileString = file.GetHostAndLocalPath();
	String dir = fileString.ExtractLastDirName();
	String fileName = fileString.ExtractFileName();
	fileName.StripFileExtension();

	// get model attributes
	const Ptr<ModelAttributes>& attrs = ModelDatabase::Instance()->LookupAttributes(dir + "/" + fileName);

	this->SetExportFlags(attrs->GetExportFlags());
	this->SetExportMode(attrs->GetExportMode());
	this->SetScale(attrs->GetScale());

	if (this->StartExport(file))
	{
		this->EndExport();
	}
}

//------------------------------------------------------------------------------
/**
*/
void NglTFExporter::ExportDir(const Util::String & dirName)
{
	String categoryDir = "src:assets/" + dirName;
	Array<String> files = IO::IoServer::Instance()->ListFiles(categoryDir, "*.fbx");
	for (int fileIndex = 0; fileIndex < files.Size(); fileIndex++)
	{
		String file = categoryDir + "/" + files[fileIndex];
		this->SetFile(files[fileIndex]);
		this->ExportFile(file);
	}
}

//------------------------------------------------------------------------------
/**
*/
void NglTFExporter::ExportAll()
{
	String workDir = "src:assets";
	Array<String> directories = IO::IoServer::Instance()->ListDirectories(workDir, "*");
	for (int directoryIndex = 0; directoryIndex < directories.Size(); directoryIndex++)
	{
		String category = workDir + "/" + directories[directoryIndex];
		this->SetCategory(directories[directoryIndex]);
		Array<String> files = IO::IoServer::Instance()->ListFiles(category, "*.fbx");
		for (int fileIndex = 0; fileIndex < files.Size(); fileIndex++)
		{
			String file = category + "/" + files[fileIndex];
			this->SetFile(files[fileIndex]);
			this->ExportFile(file);
		}
	}
}

//------------------------------------------------------------------------------
/**
	Check if we need to export. This are the dependencies we have:

	.attributes
		n3			(model resource)
		nax3		(animation resource)
	.constants
		n3			(model resource, dictates which nodes should be picked from the attributes)
		nvx2		(mesh resource)
	.physics
		np3			(physics resource)
		_ph.nvx2	(optional physics mesh resource)
*/
bool NglTFExporter::NeedsConversion(const Util::String & path)
{
	String category = path.ExtractLastDirName();
	String file = path.ExtractFileName();
	file.StripFileExtension();

	// check both if FBX is newer than .n3
	String model = "mdl:" + category + "/" + file + ".n3";
	String physModel = "phys:" + category + "/" + file + ".np3";
	String mesh = "msh:" + category + "/" + file + ".nvx2";
	String physMesh = "msh:" + category + "/" + file + "_ph.nvx2";
	String animation = "ani:" + category + "/" + file + ".nax3";
	String constants = "src:assets/" + category + "/" + file + ".constants";
	String attributes = "src:assets/" + category + "/" + file + ".attributes";
	String physics = "src:assets/" + category + "/" + file + ".physics";

	// check if fbx is newer than model
	bool fileNewer = ExporterBase::NeedsConversion(path, model);

	// and if the .constants is older than the fbx
	bool constantsNewer = ExporterBase::NeedsConversion(constants, model);

	// and if the .attributes is older than the n3 (attributes controls both model, and animation resource)
	bool attributesNewer = ExporterBase::NeedsConversion(attributes, model);

	// ...and if the .physics is older than the n3
	bool physicsNewer = ExporterBase::NeedsConversion(physics, physModel);

	// ...if the mesh is newer
	bool meshNewer = ExporterBase::NeedsConversion(path, mesh);

	// check if physics settings were changed. no way to tell if we have a new physics mesh in it, so we just export it anyway
	bool physicsMeshNewer = ExporterBase::NeedsConversion(physics, mesh);

	// return true if either is true
	return fileNewer || constantsNewer || attributesNewer || physicsNewer || meshNewer || physicsMeshNewer;
}

} // namespace ToolkitUtil
//------------------------------------------------------------------------------