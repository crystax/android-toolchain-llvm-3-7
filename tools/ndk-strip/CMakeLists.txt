set(LLVM_LINK_COMPONENTS
  asmparser
  bitreader
  bitwriter
  codegen
  instrumentation
  ipo
  irreader
  linker
  selectiondag
  )

add_llvm_tool(ndk-strip
  ndk-strip.cpp
  )
