CollectSourceFiles("${CMAKE_CURRENT_SOURCE_DIR}/src" AHBOT_SOURCES)
CollectIncludeDirectories("${CMAKE_CURRENT_SOURCE_DIR}/src" AHBOT_INCLUDES)
tc_register_module(
  NAME mod-ahbot
  SOURCES ${AHBOT_SOURCES}
  INCLUDE_DIRS ${AHBOT_INCLUDES}
  LOADER AddSC_ahbot_commandscript
  CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/conf/ahbot.conf.dist")
