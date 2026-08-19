import { Button } from "@/components/ui/button"

export function NotFound() {
  return (
    <div className="min-h-screen w-full flex items-center justify-center bg-background">
      <div className="text-center">
        <h1 className="text-4xl font-bold text-foreground">404</h1>
        <p className="mt-2 text-sm text-muted-foreground">
          Page not found.
        </p>
        <Button variant="ghost" className="mt-4" onClick={() => window.location.href = "/"}>
          Return to home
        </Button>
      </div>
    </div>
  )
}

export default NotFound;
