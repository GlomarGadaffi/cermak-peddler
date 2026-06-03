# Fix: avoid std::string_view pointer-arithmetic UB in SipMessage

This PR refactors SipMessage mutators to avoid using pointer arithmetic on std::string_view::data() when modifying the underlying message string. Instead it locates header positions using safe searches and replaces by index. This prevents undefined behavior when the backing std::string reallocates or changes.

Changes:
- SipMessage::setType, setVia, setHeader, setFrom, setTo, setCallID, setCSeq, setContact, addHeader now use findHeader/std::string::find to locate header text before replacing.
- No behavioral changes expected; safer memory semantics.

Closes: #63
