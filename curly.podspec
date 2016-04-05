Pod::Spec.new do |s|
  s.name = "curly"
  s.version = "0.4.0"
  s.summary = "High level libcurl api."
  s.homepage = "https://github.com/johanlantz/curly"
  s.license = { :type => "MIT", :file => "LICENSE" }
  s.author = "Johan Lantz"
  
  s.platform = :ios, "7.0"
  s.source = { :git => "https://github.com/johanlantz/curly.git", :tag => s.version.to_s }
  
  s.source_files  = "*.{h,c}"
  s.public_header_files = "curly.h"
  s.preserve_paths = "third-party/curl/ios/**/*"
  
  s.pod_target_xcconfig = {
    "HEADER_SEARCH_PATHS" => '"$(PODS_ROOT)/curly/third-party/curl/ios/include"',
  }
  
  s.user_target_xcconfig = {
    "LIBRARY_SEARCH_PATHS" => '$(inherited) "$(PODS_ROOT)/curly/third-party/curl/ios/lib"',
    "ENABLE_BITCODE" => "NO"
  }
  
  s.libraries = "curl"
  s.frameworks = "Foundation"
end
