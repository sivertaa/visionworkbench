// __BEGIN_LICENSE__
// 
// Copyright (C) 2006 United States Government as represented by the
// Administrator of the National Aeronautics and Space Administration
// (NASA).  All Rights Reserved.
// 
// Copyright 2006 Carnegie Mellon University. All rights reserved.
// 
// This software is distributed under the NASA Open Source Agreement
// (NOSA), version 1.3.  The NOSA has been approved by the Open Source
// Initiative.  See the file COPYING at the top of the distribution
// directory tree for the complete NOSA document.
// 
// THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF ANY
// KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT
// LIMITED TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO
// SPECIFICATIONS, ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR
// A PARTICULAR PURPOSE, OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT
// THE SUBJECT SOFTWARE WILL BE ERROR FREE, OR ANY WARRANTY THAT
// DOCUMENTATION, IF PROVIDED, WILL CONFORM TO THE SUBJECT SOFTWARE.
// 
// __END_LICENSE__

#ifdef _MSC_VER
#pragma warning(disable:4244)
#pragma warning(disable:4267)
#pragma warning(disable:4996)
#endif

#include <stdlib.h>
#include <iostream>
#include <fstream>

#include <boost/program_options.hpp>
namespace po = boost::program_options;

#include <vw/Core/Cache.h>
#include <vw/Core/ProgressCallback.h>
#include <vw/Math/Matrix.h>
#include <vw/Image/Transform.h>
#include <vw/Image/Palette.h>
#include <vw/FileIO/DiskImageResource.h>
#include <vw/FileIO/DiskImageResourceJPEG.h>
#include <vw/FileIO/DiskImageResourceGDAL.h>
#include <vw/FileIO/DiskImageView.h>
#include <vw/Cartography/GeoReference.h>
#include <vw/Cartography/GeoTransform.h>
#include <vw/Cartography/FileIO.h>
#include <vw/Mosaic/ImageComposite.h>
#include <vw/Mosaic/TMSQuadTreeGenerator.h>
#include <vw/Mosaic/UniviewQuadTreeGenerator.h>
using namespace vw;
using namespace vw::math;
using namespace vw::cartography;
using namespace vw::mosaic;

// Global Variables
std::vector<std::string> image_files;
std::string output_file_name;
std::string output_file_type;
double north_lat=90.0, south_lat=-90.0;
double east_lon=180.0, west_lon=-180.0;
double proj_lat=0, proj_lon=0, proj_scale=1;
unsigned utm_zone;
int patch_size, patch_overlap;
float jpeg_quality;
unsigned cache_size;
double nudge_x=0, nudge_y=0;
std::string palette_file, channel_type;
float palette_scale=1.0, palette_offset=0.0;

template <class ChannelT>
void do_mosaic(po::variables_map const& vm) {

  TerminalProgressCallback tpc;
  const ProgressCallback *progress = &tpc;
  if( vm.count("verbose") ) {
    set_debug_level(VerboseDebugMessage);
    progress = &ProgressCallback::dummy_instance();
  }
  else if( vm.count("quiet") ) {
    set_debug_level(WarningMessage);
  }

  DiskImageResourceJPEG::set_default_quality( jpeg_quality );
  Cache::system_cache().resize( cache_size*1024*1024 );

  GeoReference output_georef;
  output_georef.set_well_known_geogcs("WGS84");
  int total_resolution = 1024;

  // Read in georeference info and compute total resolution
  bool manual = vm.count("north") || vm.count("south") || vm.count("east") || vm.count("west");
  std::vector<GeoReference> georeferences;
  for( unsigned i=0; i<image_files.size(); ++i ) {
    std::cout << "Adding file " << image_files[i] << std::endl;
    DiskImageResourceGDAL file_resource( image_files[i] );
    GeoReference input_georef;
    read_georeference( input_georef, file_resource );

    if ( input_georef.proj4_str() == "" ) input_georef.set_well_known_geogcs("WGS84");
    if( manual || input_georef.transform() == identity_matrix<3>() ) {
      if( image_files.size() == 1 ) {
        vw_out(InfoMessage) << "No georeferencing info found.  Assuming Plate Carree WGS84: " 
                            << east_lon << " to " << west_lon << " E, " << south_lat << " to " << north_lat << " N." << std::endl;
        input_georef = GeoReference();
        input_georef.set_well_known_geogcs("WGS84");
        Matrix3x3 m;
        m(0,0) = (east_lon - west_lon) / file_resource.cols();
        m(0,2) = west_lon;
        m(1,1) = (south_lat - north_lat) / file_resource.rows();
        m(1,2) = north_lat;
        m(2,2) = 1;
        input_georef.set_transform( m );
        manual = true;
      }
      else {
        vw_out(ErrorMessage) << "Error: No georeferencing info found for input file \"" << image_files[i] << "\"!" << std::endl;
        vw_out(ErrorMessage) << "(Manually-specified bounds are only allowed for single image files.)" << std::endl;
        exit(1);
      }
    }
    else if( vm.count("sinusoidal") ) input_georef.set_sinusoidal(proj_lon);
    else if( vm.count("mercator") ) input_georef.set_mercator(proj_lat,proj_lon,proj_scale);
    else if( vm.count("transverse-mercator") ) input_georef.set_transverse_mercator(proj_lat,proj_lon,proj_scale);
    else if( vm.count("orthographic") ) input_georef.set_orthographic(proj_lat,proj_lon);
    else if( vm.count("stereographic") ) input_georef.set_stereographic(proj_lat,proj_lon,proj_scale);
    else if( vm.count("lambert-azimuthal") ) input_georef.set_lambert_azimuthal(proj_lat,proj_lon);
    else if( vm.count("utm") ) input_georef.set_UTM( utm_zone );

    if( vm.count("nudge-x") || vm.count("nudge-y") ) {
      Matrix3x3 m = input_georef.transform();
      m(0,2) += nudge_x;
      m(1,2) += nudge_y;
      input_georef.set_transform( m );
    }
    
    georeferences.push_back( input_georef );

    GeoTransform geotx( input_georef, output_georef );
    Vector2 center_pixel( file_resource.cols()/2, file_resource.rows()/2 );
    int resolution = GlobalTMSTransform::compute_resolution( geotx, center_pixel );
    if( resolution > total_resolution ) total_resolution = resolution;
  }

  // Configure the composite
  ImageComposite<PixelRGBA<ChannelT> > composite;
  GlobalTMSTransform tmstx( total_resolution );

  // Add the transformed input files to the composite
  for( unsigned i=0; i<image_files.size(); ++i ) {
    GeoTransform geotx( georeferences[i], output_georef );
    ImageViewRef<PixelRGBA<ChannelT> > source = DiskImageView<PixelRGBA<ChannelT> >( image_files[i] );
    if( vm.count("palette-file") ) {
      DiskImageView<float> disk_image( image_files[i] );
      if( vm.count("palette-scale") || vm.count("palette-offset") ) {
        source.reset( per_pixel_filter( disk_image*palette_scale+palette_offset, PaletteFilter<PixelRGBA<ChannelT> >(palette_file) ) );
      }
      else {
        source.reset( per_pixel_filter( disk_image, PaletteFilter<PixelRGBA<ChannelT> >(palette_file) ) );
      }
    }
    BBox2i bbox = compose(tmstx,geotx).forward_bbox( BBox2i(0,0,source.cols(),source.rows()) );
    // Constant edge extension is better for transformations that 
    // preserve the rectangularity of the image.  At the moment we 
    // only do this for manual transforms, alas.
    if( manual ) {
      // If the image is being super-sampled the computed bounding 
      // box may be missing a pixel at the edges relative to what 
      // you might expect, which can create visible artifacts if 
      // it happens at the boundaries of the coordinate system.
      if( west_lon == -180 ) bbox.min().x() = 0;
      if( east_lon == 180 ) bbox.max().x() = total_resolution;
      if( north_lat == 90 ) bbox.min().y() = total_resolution/2;
      if( south_lat == -90 ) bbox.max().y() = total_resolution;
      source = crop( transform( source, compose(tmstx,geotx), ConstantEdgeExtension() ), bbox );
    }
    else {
      source = crop( transform( source, compose(tmstx,geotx) ), bbox );
    }
    composite.insert( source, bbox.min().x(), bbox.min().y() );
    // Images that wrap the date line must be added to the composite on both sides.
    if( bbox.max().x() > total_resolution ) {
      composite.insert( source, bbox.min().x()-total_resolution, bbox.min().y() );
    }
  }

  // Grow the bounding box to align it with the patch size boundaries
  BBox2i bbox = composite.bbox();
  std::cout << "Comp bbox: " << bbox << "\n";
  BBox2i data_bbox(std::floor(double(bbox.min().x())/patch_size)*patch_size,
                   std::floor(double(bbox.min().y())/patch_size)*patch_size,
                   std::ceil(double(bbox.width())/patch_size)*patch_size,
                   std::ceil(double(bbox.height())/patch_size)*patch_size);
  std::cout << "Data bbox: " << data_bbox << "\n";

  BBox2i total_bbox = composite.bbox();
  total_bbox.grow( BBox2i(0,0,total_resolution,total_resolution) );
  std::cout << "Total bbox: " << total_bbox << "\n";
  std::cout << "Total res: " << total_resolution << "\n";

  // Prepare the composite
  if( vm.count("composite-multiband") ) {
    std::cout << "Preparing composite..." << std::endl;
    composite.prepare( total_bbox, *progress );
  }
  else {
    composite.set_draft_mode( true );
    composite.prepare( total_bbox );
  }


  // Compute the geodetic bounding box
  Vector2 invmin = tmstx.reverse(total_bbox.min());
  Vector2 invmax = tmstx.reverse(total_bbox.max());
  BBox2 ll_bbox;
  ll_bbox.min().x() = invmin[0];
  ll_bbox.max().y() = invmin[1];
  ll_bbox.max().x() = invmax[0];
  ll_bbox.min().y() = invmax[1];
  vw_out(InfoMessage) << "LonLat BBox: " << ll_bbox << "\n";

  // Prepare the quadtree
  if (vm.count("uniview")) {
    UniviewQuadTreeGenerator<PixelRGBA<ChannelT> > quadtree( output_file_name, composite );
    quadtree.set_crop_bbox( data_bbox );
    if( vm.count("crop") ) quadtree.set_crop_images( true );
    quadtree.set_output_image_file_type( output_file_type );
    quadtree.set_patch_size( patch_size );

    // Generate the composite
    vw_out(InfoMessage) << "Generating Uniview Overlay..." << std::endl;
    quadtree.generate( *progress );

    std::string config_filename = output_file_type + ".conf";
    std::ofstream conf( config_filename.c_str());
    conf << "[Offlinedataset]\n";
    conf << "NrRows=1\n";
    conf << "NrColumns=2\n";
    //    conf << "Bbox= "<<ll_bbox.min().x()<<" "<<ll_bbox.max().y()<<" "<<ll_bbox.max().x()<<" "<<ll_bbox.min().y()<<"\n";
    conf << "Bbox= -180 -90 180 90\n";
    conf << "DatasetTitle=" << output_file_name << "\n";
    conf << "Tessellation=19\n\n";

    conf << "// Texture\n";
    conf << "TextureCacheLocation=modules/marsds/Offlinedatasets/" << output_file_name << "/Texture/\n";
    conf << "TextureCallstring=Generated by the NASA Vision Workbench image2tms tool.\n";
    conf << "TextureFormat=" << quadtree.get_output_image_file_type() << "\n";
    conf << "TextureLevels= " << quadtree.get_tree_levels()-1 << "\n";
    conf << "TextureSize= " << patch_size << "\n\n";
    conf.close();

  } else {
    TMSQuadTreeGenerator<PixelRGBA<ChannelT> > quadtree( output_file_name, composite );
    quadtree.set_crop_bbox( data_bbox );
    if( vm.count("crop") ) quadtree.set_crop_images( true );
    quadtree.set_output_image_file_type( output_file_type );
    quadtree.set_patch_size( patch_size );
    
    // Generate the composite
    vw_out(InfoMessage) << "Generating TMS Overlay..." << std::endl;
    quadtree.generate( *progress );
  }  
}


int main( int argc, char *argv[] ) {

  po::options_description general_options("General Options");
  general_options.add_options()
    ("output-name,o", po::value<std::string>(&output_file_name)->default_value("output"), "Specify the base output filename")
    ("quiet,q", "Quiet output")
    ("verbose,v", "Verbose output")
    ("uniview", "Produce output suitable for use with the SCISS Uniview Program.")
    ("cache", po::value<unsigned>(&cache_size)->default_value(1024), "Cache size, in megabytes")
    ("help", "Display this help message");

  po::options_description projection_options("Projection Options");
  projection_options.add_options()
    ("north", po::value<double>(&north_lat), "The northernmost latitude in degrees")
    ("south", po::value<double>(&south_lat), "The southernmost latitude in degrees")
    ("east", po::value<double>(&east_lon), "The easternmost latitude in degrees")
    ("west", po::value<double>(&west_lon), "The westernmost latitude in degrees")
    ("sinusoidal", "Assume a sinusoidal projection")
    ("mercator", "Assume a Mercator projection")
    ("transverse-mercator", "Assume a transverse Mercator projection")
    ("orthographic", "Assume an orthographic projection")
    ("stereographic", "Assume a stereographic projection")
    ("lambert-azimuthal", "Assume a Lambert azimuthal projection")
    ("utm", po::value<unsigned>(&utm_zone), "Assume UTM projection with the given zone")
    ("proj-lat", po::value<double>(&proj_lat), "The center of projection latitude (if applicable)")
    ("proj-lon", po::value<double>(&proj_lon), "The center of projection longitude (if applicable)")
    ("proj-scale", po::value<double>(&proj_scale), "The projection scale (if applicable)")
    ("nudge-x", po::value<double>(&nudge_x), "Nudge the image, in projected coordinates")
    ("nudge-y", po::value<double>(&nudge_y), "Nudge the image, in projected coordinates");
    
  po::options_description output_options("Output Options");
  output_options.add_options()
    ("file-type", po::value<std::string>(&output_file_type)->default_value("png"), "Output file type")
    ("channel-type", po::value<std::string>(&channel_type)->default_value("uint8"), "Output channel type.  Choose one of: [uint8, uint16, int16, float]")
    ("jpeg-quality", po::value<float>(&jpeg_quality)->default_value(0.75), "JPEG quality factor (0.0 to 1.0)")
    ("palette-file", po::value<std::string>(&palette_file), "Apply a palette from the given file")
    ("palette-scale", po::value<float>(&palette_scale), "Apply a scale factor before applying the palette")
    ("palette-offset", po::value<float>(&palette_offset), "Apply an offset before applying the palette")
    ("patch-size", po::value<int>(&patch_size)->default_value(256), "Patch size, in pixels")
    ("patch-overlap", po::value<int>(&patch_overlap)->default_value(0), "Patch overlap, in pixels (must be even)")
    ("patch-crop", "Crop output patches")
    ("composite-overlay", "Composite images using direct overlaying (default)")
    ("composite-multiband", "Composite images using multi-band blending");

  po::options_description hidden_options("");
  hidden_options.add_options()
    ("input-file", po::value<std::vector<std::string> >(&image_files));

  po::options_description options("Allowed Options");
  options.add(general_options).add(projection_options).add(output_options).add(hidden_options);

  po::positional_options_description p;
  p.add("input-file", -1);

  po::variables_map vm;
  po::store( po::command_line_parser( argc, argv ).options(options).positional(p).run(), vm );
  po::notify( vm );

  std::ostringstream usage;
  usage << "Usage: image2tms [options] <filename>..." << std::endl << std::endl;
  usage << general_options << std::endl;
  usage << output_options << std::endl;
  usage << projection_options << std::endl;

  if( vm.count("help") ) {
    std::cout << usage.str();
    return 1;
  }

  if( vm.count("input-file") < 1 ) {
    std::cout << "Error: Must specify at least one input file!" << std::endl << std::endl;
    std::cout << usage.str();
    return 1;
  }

  if( patch_size <= 0 ) {
    std::cerr << "Error: The patch size must be a positive number!  (You specified " << patch_size << ".)" << std::endl << std::endl;
    std::cout << usage.str();
    return 1;
  }
    
  if( patch_overlap<0 || patch_overlap>=patch_size || patch_overlap%2==1 ) {
    std::cerr << "Error: The patch overlap must be an even nonnegative number" << std::endl;
    std::cerr << "smaller than the patch size!  (You specified " << patch_overlap << ".)" << std::endl << std::endl;
    std::cout << usage.str();
    return 1;
  }

  if (channel_type == "uint8") 
    do_mosaic<uint8>(vm);
  else if (channel_type == "uint16") 
    do_mosaic<uint16>(vm);
  else if (channel_type == "int16") 
    do_mosaic<int16>(vm);
  else if (channel_type == "float") 
    do_mosaic<float>(vm);
  else 
    std::cout << "Unknown channel type: " << channel_type << "     Choose a channel type from: [uint8, uint16, int16, float]\n";

  return 0;
}