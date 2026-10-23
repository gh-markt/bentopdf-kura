add_executable(test_validation validation.cpp)
target_link_libraries(test_validation PRIVATE pdfa_core)
target_include_directories(test_validation PRIVATE ../core/src)
foreach(case IN ITEMS
    metadata_control metadata_missing metadata_malformed metadata_wrong_part
    metadata_wrong_namespace metadata_alternate_prefix metadata_attributes
    metadata_duplicate metadata_doctype metadata_encoding metadata_stream_type
    metadata_depth metadata_size metadata_wrong_root metadata_other_subject
    metadata_integer metadata_legacy_x1a metadata_legacy_x3
    content_control content_unknown content_compatibility content_nested_compatibility
    content_unmatched_begin content_unmatched_end content_split_streams
    content_comments_strings content_inline_image content_form content_pattern
    content_type3 content_appearance content_resource_cycle content_conversion_reject
    dpart_control dpart_optional_types dpart_parent dpart_child_cycle
    dpart_shared_child dpart_start dpart_reversed dpart_page_owner
    dpart_overlap dpart_multiple_leaves dpart_nested dpart_repair
    integration_missing_metadata integration_malformed_metadata
    integration_unknown_operator integration_dpart_parent)
  add_test(NAME kura.validation.${case} COMMAND test_validation ${case})
  set_tests_properties(kura.validation.${case} PROPERTIES TIMEOUT 30)
endforeach()
