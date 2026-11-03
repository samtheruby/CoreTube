project('gst-amlvideosink', 'c',
  version: '1.0',
  license: 'LGPL-2.1-or-later',
  meson_version: '>= 1.4',
  default_options: ['warning_level=2'])

gst_dep = dependency('gstreamer-1.0', version: '>= 1.20')
gstbase_dep = dependency('gstreamer-base-1.0', version: '>= 1.20')
gstvideo_dep = dependency('gstreamer-video-1.0', version: '>= 1.20')
dl_dep = dependency('dl')
# libamcodec is loaded at run time (see load_amcodec); only its headers are needed here.
# They include each other as <codec_type.h>, so their folder must be on the path.
amcodec_inc = meson.get_external_property('root') / 'include' / 'amcodec'

shared_module('gstamlvideosink', 'gstamlvideosink.c', 'aml_pacer.c',
  c_args: ['-I' + amcodec_inc,
           '-DPACKAGE="@0@"'.format(meson.project_name()),
           '-DVERSION="@0@"'.format(meson.project_version())],
  dependencies: [gst_dep, gstbase_dep, gstvideo_dep, dl_dep],
  install: true,
  install_dir: get_option('libdir') / 'gstreamer-1.0')
