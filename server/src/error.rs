use axum::{http::StatusCode, response::{IntoResponse, Response}, Json};
use serde_json::json;

#[derive(Debug,Clone)]
pub struct Error { pub status:u16,pub message:String }
pub type Result<T> = std::result::Result<T,Error>;

impl Error {
    // Public failures contain actionable messages without upstream credentials or SQL data.
    pub fn new(status:u16,message:impl Into<String>) -> Self { Self {status,message:message.into()} }
}
impl std::fmt::Display for Error {
    // Only the safe public description is printable.
    fn fmt(&self,f:&mut std::fmt::Formatter<'_>) -> std::fmt::Result {write!(f,"{}",self.message)}
}
impl std::error::Error for Error {}
impl From<rusqlite::Error> for Error {
    // Storage errors stay generic at the HTTP boundary.
    fn from(_:rusqlite::Error) -> Self {Self::new(500,"Storage error. Please contact support.")}
}
impl IntoResponse for Error {
    // Authentication, billing and provider failures use the client's existing JSON contract.
    fn into_response(self) -> Response {(StatusCode::from_u16(self.status).unwrap(),Json(json!({"error":self.message}))).into_response()}
}
