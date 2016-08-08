package websockets

import (
	"errors"
	"strings"
	"unicode/utf8"

	"github.com/bakape/meguca/db"
	"github.com/bakape/meguca/parser"
	"github.com/bakape/meguca/types"
	"github.com/bakape/meguca/util"
	r "github.com/dancannon/gorethink"
)

var (
	errNoPostOpen          = errors.New("no post open")
	errLineEmpty           = errors.New("line empty")
	errInvalidSpliceCoords = errors.New("invalid splice coordinates")
	errSpliceTooLong       = errors.New("splice text too long")
	errNewlineInSplice     = errors.New("newline in splice text")
	errSpliceNOOP          = errors.New("splice NOOP")
)

// Request or response to replace the current line's text starting at an exact
// position in the current line
type spliceMessage struct {
	Start int    `json:"start"`
	Len   int    `json:"len"`
	Text  string `json:"text"`
}

// Shorthand. We use it a lot for update query construction.
type msi map[string]interface{}

// Append a rune to the body of the open post
func appendRune(data []byte, c *Client) error {
	if !c.hasPost() {
		return errNoPostOpen
	}
	if c.openPost.bodyLength+1 > parser.MaxLengthBody {
		return parser.ErrBodyTooLong
	}
	var char rune
	if err := decodeMessage(data, &char); err != nil {
		return err
	}

	if char == '\n' {
		return parseLine(c, true)
	}

	id := c.openPost.id
	msg, err := encodeMessage(messageAppend, [2]int64{id, int64(char)})
	if err != nil {
		return err
	}

	update := msi{
		"body": postBody(id).Add(string(char)),
	}
	if err := c.updatePost(update, msg); err != nil {
		return err
	}
	c.openPost.WriteRune(char)
	c.openPost.bodyLength++

	return nil
}

// Shorthand for retrievinf the post's body field from a thread document
func postBody(id int64) r.Term {
	return r.Row.
		Field("posts").
		Field(util.IDToString(id)).
		Field("body")
}

// Helper for running post update queries on the current open post
func (c *Client) updatePost(update msi, msg []byte) error {
	q := r.
		Table("threads").
		Get(c.openPost.op).
		Update(createUpdate(c.openPost.id, update, msg))
	return db.Write(q)
}

// Helper for creating post update maps
func createUpdate(id int64, update msi, msg []byte) msi {
	return msi{
		"log": appendLog(msg),
		"posts": msi{
			util.IDToString(id): update,
		},
	}
}

// Shorthand for creating a replication log append query
func appendLog(msg []byte) r.Term {
	return r.Row.Field("log").Append(msg)
}

// Parse line contents and commit newline. If line contains hash commands or
// links to other posts also commit those and generate backlinks, if needed.
// Appending the newline can be optionally omitted, to optimise post closing
// and similar.
func parseLine(c *Client, insertNewline bool) error {
	c.openPost.bodyLength++
	links, comm, err := parser.ParseLine(c.openPost.Bytes(), c.openPost.board)
	if err != nil {
		return err
	}
	defer c.openPost.Reset()
	idStr := util.IDToString(c.openPost.id)

	if insertNewline {
		msg, err := encodeMessage(messageAppend, [2]int64{
			c.openPost.id,
			int64('\n'),
		})
		if err != nil {
			return err
		}
		update := msi{
			"body": r.Row.
				Field("posts").
				Field(idStr).
				Field("body").
				Add("\n"),
		}
		if err := c.updatePost(update, msg); err != nil {
			return err
		}
	}

	switch {
	case comm.Val != nil:
		return writeCommand(comm, idStr, c)
	case links != nil:
		return writeLinks(links, c)
	default:
		return nil
	}
}

// Write a hash command to the database
func writeCommand(comm types.Command, idStr string, c *Client) error {
	msg, err := encodeMessage(messageCommand, comm)
	if err != nil {
		return err
	}
	update := msi{
		"commands": r.Row.
			Field("posts").
			Field(idStr).
			Field("commands").
			Default([]types.Command{}).
			Append(comm),
	}
	return c.updatePost(update, msg)
}

// Write new links to other posts to the database
func writeLinks(links types.LinkMap, c *Client) error {
	msg, err := encodeMessage(messageLink, links)
	if err != nil {
		return err
	}
	update := msi{
		"links": links,
	}
	if err := c.updatePost(update, msg); err != nil {
		return err
	}

	// Most often this loop will iterate only once, so no need to think heavily
	// about optimisations
	for destID := range links {
		id := c.openPost.id
		op := c.openPost.op
		board := c.openPost.board
		if err := writeBacklink(id, op, board, destID); err != nil {
			return err
		}
	}

	return nil
}

// Writes the location data of the post linking a post to the the post being
// linked
func writeBacklink(id, op int64, board string, destID int64) error {
	msg, err := encodeMessage(messageBacklink, types.LinkMap{
		id: {
			OP:    op,
			Board: board,
		},
	})
	if err != nil {
		return err
	}

	update := msi{
		"backlinks": msi{
			util.IDToString(id): types.Link{
				OP:    op,
				Board: board,
			},
		},
	}
	q := r.
		Table("threads").
		GetAllByIndex("post", destID).
		Update(createUpdate(destID, update, msg))

	return db.Write(q)
}

// Remove one character from the end of the line in the open post
func backspace(_ []byte, c *Client) error {
	if !c.hasPost() {
		return errNoPostOpen
	}
	length := c.openPost.Len()
	if length == 0 {
		return errLineEmpty
	}
	_, lastRuneLen := utf8.DecodeLastRune(c.openPost.Bytes())
	c.openPost.Truncate(length - lastRuneLen)
	c.openPost.bodyLength--

	id := c.openPost.id
	update := msi{
		"body": postBody(id).Slice(0, -1),
	}
	msg, err := encodeMessage(messageBackspace, id)
	if err != nil {
		return err
	}
	return c.updatePost(update, msg)
}

// Close an open post and parse the last line, if needed.
func closePost(_ []byte, c *Client) error {
	if !c.hasPost() {
		return errNoPostOpen
	}
	if c.openPost.Len() != 0 {
		if err := parseLine(c, false); err != nil {
			return err
		}
	}

	defer func() {
		c.openPost = openPost{}
	}()
	update := msi{
		"editing": false,
	}
	msg, err := encodeMessage(messageClosePost, c.openPost.id)
	if err != nil {
		return err
	}
	return c.updatePost(update, msg)
}

// Splice the current line's text in the open post. This call is also used for
// text pastes.
func spliceLine(data []byte, c *Client) error {
	if !c.hasPost() {
		return errNoPostOpen
	}

	old := c.openPost.String()

	var req spliceMessage
	err := decodeMessage(data, &req)
	switch {
	case err != nil:
		return err
	case req.Start < 0, req.Len < 0, req.Start+req.Len > len(old):
		return errInvalidSpliceCoords
	case req.Len == 0 && req.Text == "":
		return errSpliceNOOP // This does nothing. Client-side error.
	case len(req.Text) > parser.MaxLengthBody:
		return errSpliceTooLong // Nice try, kid
	case strings.ContainsRune(req.Text, '\n'):
		// To reduce complexity force the client to split multiline splices
		return errNewlineInSplice
	}

	new := old[:req.Start] + req.Text + old[req.Start+req.Len:]
	c.openPost.bodyLength += -req.Len + len(req.Text)

	// Goes over max post length. Trim the end.
	if c.openPost.bodyLength > parser.MaxLengthBody {
		exceeding := c.openPost.bodyLength - parser.MaxLengthBody
		new = new[:len(new)-exceeding]
		req.Len = -1 // Special meaning. Client should replace till line end.
		req.Text = new
		c.openPost.bodyLength = parser.MaxLengthBody
	}

	c.openPost.Reset()
	c.openPost.WriteString(new)

	msg, err := encodeMessage(messageSplice, req)
	if err != nil {
		return err
	}

	// Split body into lines, remove last line and replace with new text
	update := msi{
		"body": postBody(c.openPost.id).
			Split("\n").
			Do(func(b r.Term) r.Term {
				return b.
					Slice(0, -1).
					Append(new).
					Fold("", func(all, line r.Term) r.Term {
						return all.Add(
							all.Eq("").Branch(
								line,
								r.Expr("\n").Add(line),
							),
						)
					})
			}),
	}

	return c.updatePost(update, msg)
}
