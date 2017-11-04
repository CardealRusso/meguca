#pragma once

#include "../../brunhild/view.hh"
#include "../json.hh"
#include <map>
#include <optional>
#include <stdint.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using brunhild::Node;

// Possible file types of a post image or thumbnail
enum class FileType : uint8_t {
    jpg,
    png,
    gif,
    webm,
    pdf,
    svg,
    mp4,
    mp3,
    ogg,
    zip,
    _7z,
    targz,
    tarxz,
    flac,
    no_file,
    txt,
};

// Maps file_type to the appropriate file extension
const std::unordered_map<FileType, std::string> file_extentions = {
    { FileType::jpg, "jpg" }, { FileType::png, "png" },
    { FileType::gif, "gif" }, { FileType::webm, "webm" },
    { FileType::pdf, "pdf" }, { FileType::svg, "svg" },
    { FileType::mp4, "mp4" }, { FileType::mp3, "mp3" },
    { FileType::ogg, "ogg" }, { FileType::zip, "zip" }, { FileType::_7z, "7z" },
    { FileType::targz, "tar.gz" }, { FileType::tarxz, "tar.xz" },
    { FileType::flac, "flac" }, { FileType::txt, "txt" },
};

class Image {
public:
    bool apng = false, // PNG image is APNG
        audio = false, // Has audio
        video = false, // Has video
        spoiler = false, // Is spoilered
        expanded = false, // Expand image thumbnail to full view
        taller_than_viewport = false, // Image is taller than the viewport
        reveal_thumbnail = false; // Reveal a hidden image with [Show]
    FileType file_type, // File type of source file
        thumb_type; // File type of thumbnail
    uint16_t dims[4];
    uint32_t length = 0; // Length of media, if a media file
    uint64_t size;
    std::optional<std::string> artist, // Media file artist meta info
        title; // Media file title meta info
    std::string MD5, // MD5 hash of source file
        SHA1, // SHA1 hash of source file
        name; // Name the file was uploaded with

    Image() = default;

    // Parse from JSON
    Image(nlohmann::json&);

    // Returns the path to this files's thumbnail
    std::string thumb_path() const;

    // Returns the path to the source file
    std::string source_path() const;

private:
    // Returns the root hosting address of all images
    std::string image_root() const;
};

// Single hash command result delivered from the server
class Command {
public:
    // Indicates the contained type
    enum class Type { dice, flip, eight_ball, sync_watch, pyu, pcount } typ;

    // Use typ, to get out the relevant value
    bool flip; // Result of flip command
    uint64_t count; // Somekind of counter result
    uint64_t sync_watch[5]; // Syncwatch parameters
    std::vector<uint16_t> dice; // Result of dice throw
    std::string eight_ball; // Result of #8ball command

    // Parse from JSON
    Command(nlohmann::json&);
};

// Data associated with link to another post. Is always pared in a map with
// the ID of the linked post as a key.
struct LinkData {
    // The post and its subtree is now a child of the link
    bool is_inlined = false;
    // Parent thread ID of the post
    uint64_t op;
};

// State of a post's text. Used for adding enclosing tags to the HTML while
// parsing.
class TextState {
public:
    bool spoiler = false, // Current text is spoilered
        quote = false, // Current line is spoilered
        code = false, // Text is inside code block
        bold = false, // Text inside bold tag
        italic = false, // Text inside italic tag
        have_syncwatch = false; // Text contains #syncwatch command(s)
    int successive_newlines = 0, // Number of successive newlines in text
        dice_index = 0; // Index of the next dice array item to use

    // Reset to initial values and sets Node as the new root parent.
    void reset(Node* root);

    // Append a Node to the current lowermost parent.
    // If descend = true, make it the next parent to append to.
    void append(Node n, bool descend = false);

    // Acsend one level up the parent tree and make it the next node to append
    // to
    void ascend() { parents.pop_back(); }

private:
    // Last child nodes of the blockquote subtree.
    // Used to keep track of nodes to append to, while populating the
    // subtree.
    std::vector<Node*> parents;
};

// Generic post model
class Post : public brunhild::VirtualView {
public:
    bool editing = false, // Post is currrently being edited
        deleted = false, // Deleted by moderator
        sage = false, // Poster disabled bumping of the parent thread
        banned = false, // Banned for this post by moderator
        sticky = false, // Thread is stickied. Only for OPs.
        locked = false, // Thread is locked. Only for OPs.
        seen = false, // The user has already seen this post
        hidden = false, // The post has been hidden by the user
        is_rendered = false; // Is Post currently represented inside the DOM?
    std::optional<Image> image;
    uint64_t id, op;
    time_t time;
    std::string body, board;
    std::optional<std::string> name, // Name of poster
        trip, // Trip code of poster
        auth, // Staff title of poster
        subject, // Subject of thread. Only for OPs.
        flag, // Country code of poster
        poster_id; // Thread-level poster ID
    std::vector<Command> commands; // Results of hash commands
    std::map<uint64_t, LinkData> backlinks; // Posts linking to this post
    std::unordered_map<uint64_t, LinkData> links; // Posts linked by this post

    Post() = default;

    // Parse from JSON
    Post(nlohmann::json&);

    // Generates the model's node tree
    Node render();

private:
    TextState state;

    // Render the header on top of the post
    Node render_header();

    // Render the name and tripcode in the header
    Node render_name();

    // Renders a time element. Can be either absolute or relative.
    Node render_time();

    // Render the information caption above the image.
    Node render_figcaption();

    // Render reverse image search links
    Node render_image_search();

    // Render uploaded file meta information
    Node render_file_info();

    // Render a thumbnail or expanded source media content
    Node render_image();

    // Render the text body of a post
    Node render_body();

    // Parse temporary links in open posts, that still may be edited
    void parse_temp_links(std::string_view);

    // Parse a line fragment into an HTML subtree
    void parse_fragment(std::string_view);

    // Highlight common programming code syntax
    void highlight_syntax(std::string_view);

    // Split string_view into subviews and run either on_match or filler on
    // the fragments appropriately.
    template <class F_M, class F_UM>
    void parse_string(
        std::string_view frag, const std::string sep, F_UM filler, F_M on_match)
    {
        while (1) {
            const size_t i = frag.find(sep);
            if (i != -1) {
                filler(frag.substr(0, i));
                frag = frag.substr(i + sep.size());
                on_match();
            } else {
                filler(frag);
                break;
            }
        }
    }

    // Detect and format code tags. Call fn on unmatched sub-fragments.
    template <class F> void parse_code(std::string_view frag, F fn)
    {
        parse_string(frag, "``",
            [this, fn](std::string_view frag) {
                if (state.code) {
                    // Strip quotes
                    size_t num_quotes = 0;
                    while (frag.size() && frag[0] == '>') {
                        frag = frag.substr(1);
                    }
                    if (num_quotes) {
                        std::string s;
                        s.reserve(4 * num_quotes);
                        for (int i = 0; i <= num_quotes; i++) {
                            s += "&gt;";
                        }
                        state.append({ "span", s });
                    }

                    highlight_syntax(frag);
                } else {
                    parse_spoilers(frag, fn);
                }
            },
            [this]() { state.code = !state.code; });
    }

    // Inject spoiler tags and call fn on the remaining parts
    template <class F> void parse_spoilers(std::string_view frag, F fn)
    {
        parse_string(frag, "**",
            [this, fn](std::string_view frag) { parse_bolds(frag, fn); },
            [this]() {
                if (state.italic) {
                    state.ascend();
                }
                if (state.bold) {
                    state.ascend();
                }

                if (state.spoiler) {
                    state.ascend();
                } else {
                    state.append({ "del" }, true);
                }

                if (state.bold) {
                    state.append({ "b" }, true);
                }
                if (state.italic) {
                    state.append({ "i" }, true);
                }

                state.spoiler = !state.spoiler;
            });
    }

    // Inject bold tags and call fn on the remaining parts
    template <class F> void parse_bolds(std::string_view frag, F fn)
    {
        parse_string(frag, "__",
            [this, fn](std::string_view frag) { parse_italics(frag, fn); },
            [this]() {
                if (state.italic) {
                    state.ascend();
                }

                if (state.bold) {
                    state.ascend();
                } else {
                    state.append({ "b" }, true);
                }

                if (state.italic) {
                    state.append({ "i" }, true);
                }

                state.bold = !state.bold;
            });
    }

    // Inject italic tags and call fn on the remaining parts
    template <class F> void parse_italics(std::string_view frag, F fn)
    {
        parse_string(frag, "~~", fn, [this]() {
            if (state.italic) {
                state.ascend();
            } else {
                state.append({ "i" }, true);
            }

            state.italic = !state.italic;
        });
    }
};

// Contains thread metadata
class Thread {
public:
    uint64_t post_ctr, // Number of posts in thread
        image_ctr, // Number of images in thread
        reply_time, // Unix timestamp of last reply
        bump_time; // Unix timestamp of last bump
};