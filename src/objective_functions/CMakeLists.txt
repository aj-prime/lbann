# Add the source files for this directory
set_full_path(THIS_DIR_SOURCES
  objective_function.cpp
  objective_function_term.cpp
  layer_term.cpp
  )

# Add the subdirectories
add_subdirectory(weight_regularization)

# Propagate the files up the tree
set(SOURCES "${SOURCES}" "${THIS_DIR_SOURCES}" PARENT_SCOPE)
set(GPU_SOURCES "${GPU_SOURCES}" "${THIS_DIR_CU_SOURCES}" PARENT_SCOPE)
