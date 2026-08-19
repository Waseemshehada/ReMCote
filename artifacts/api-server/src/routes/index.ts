import { Router, type IRouter } from "express";
import healthRouter from "./health";
import remcoteRouter from "./remcote";

const router: IRouter = Router();

router.use(healthRouter);
router.use(remcoteRouter);

export default router;
